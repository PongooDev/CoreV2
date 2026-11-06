#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "AthenaPlayerMatchReport.h"
#include "BackendRequest.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/PlayerState.h"
#include "Logging/LogMacros.h"
#include "ObjectLayout.h"
#include "StatManager.h"
#include "Templates/Casts.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

// The Battle Royale end-of-match screen (AthenaMatchStatsPartial, "Your Results") shows the controller's match report
// (UAthenaPlayerMatchReport): its stats come with ClientSendMatchStatsForPlayer and its rewards, which end the "Team
// Still Alive" wait, with ClientSendEndBattleRoyaleMatchForPlayer. 1.10's server code keeps everything the screen shows
// in the controller's StatManager and player state (StatManager.h) but no code sends either RPC (no code names them),
// so the screen showed zeros (#162, #167). This is the sender, which 21.40 still has as
// AFortPlayerController::SendMatchStatsForPlayer: it reads the match's values (period Map) into the server's own
// report, whose bHasStats says the player's stats are out (21.40's bMatchStatsForPlayerSent), and sends them once.
// 1.11 keeps the place and total players apart (FAthenaMatchTeamStats, ClientSendTeamStatsForPlayer, no code sending
// it either): its screen writes them when the rewards come, so they go between the stats and the rewards.

DEFINE_LOG_CATEGORY_STATIC(LogFortMatchStats, Log, All);

namespace
{
    // Assists: other teams' players this player hit (HostilePlayersDamaged) whom someone else eliminated.
    int32_t CountAssists(AFortPlayerStateAthena* PlayerState)
    {
        if (!AFortPlayerStateAthena::HasHostilePlayersDamaged())
        {
            return 0;
        }
        int32_t Assists = 0;
        const TSet<AFortPlayerStateAthena*>& Damaged = PlayerState->GetHostilePlayersDamaged();
        for (AFortPlayerStateAthena* Victim : Damaged)
        {
            AFortPlayerStateAthena* Eliminator = Victim ? Victim->GetEliminator() : nullptr;
            if (Eliminator && Eliminator != Victim && Eliminator != PlayerState)
            {
                ++Assists;
            }
        }
        return Assists;
    }

    // Hits: the hits the player's guns landed on other teams' pawns. Epic's "Ranged.Hit.Hostile:<source>"
    // (AFortPlayerPawnAthena's damage handler, 3.0 0x9ED290, 3.3 0xA212E0) counts a hit only when the victim's
    // controller is an AFortPlayerControllerAthena, so Core's bots (an AAIController each) never count, nor do they go
    // in HostilePlayersDamaged there: a match against bots read Hits 0 and Accuracy 0% while their damage counted
    // (#236). AFortPawn's damage handler (2.5 0xD991C0, 3.0 0xDBB500, 3.1 0xDF8C50, 3.2 0xE0DA60, 3.3 0xE59470), the one
    // that adds "Damage.Dealt.ToHostile", counts every weapon's hit on another team's pawn, whoever controls it, as
    // "Hit.Zone.Normal.ToHostile:<source>" or "Hit.Zone.Critical.ToHostile:<source>" (one each, by the hit's damage
    // zone), under the same weapon's AnalyticTags. The guns are the sources the player fired from ("Ranged.Bullets.Fired",
    // 3.0 0x8141D0, or 1.10's "Ranged.Fire"), which leaves out the pickaxe's hits; a build without the zone stats keeps
    // Epic's count.
    int32_t CountRangedHits(UStatManager* Manager, EStatRecordingPeriod Period)
    {
        static const std::wstring FiredPrefixes[] = { L"Ranged.Bullets.Fired:", L"Ranged.Fire:" };
        static const std::wstring HitPrefixes[] = { L"Hit.Zone.Normal.ToHostile:", L"Hit.Zone.Critical.ToHostile:" };
        // Names compare without case, as FNames do.
        const auto SourceAfter = [](const std::wstring& Name, const std::wstring& Prefix)
        {
            if (Name.size() <= Prefix.size() || _wcsnicmp(Name.c_str(), Prefix.c_str(), Prefix.size()) != 0)
            {
                return std::wstring();
            }
            std::wstring Source = Name.substr(Prefix.size());
            std::transform(Source.begin(), Source.end(), Source.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
            return Source;
        };

        std::vector<std::wstring> Guns;
        std::vector<std::pair<std::wstring, FName>> Hits;
        const TArray<UStat*>& AllStats = Manager->Stats;
        for (UStat* Each : AllStats)
        {
            if (!Each)
            {
                continue;
            }
            const FName Name = static_cast<FName>(Each->StatName);
            const std::wstring Text = Name.ToStdString();
            for (const std::wstring& Prefix : FiredPrefixes)
            {
                if (std::wstring Source = SourceAfter(Text, Prefix); !Source.empty())
                {
                    Guns.push_back(std::move(Source));
                }
            }
            for (const std::wstring& Prefix : HitPrefixes)
            {
                if (std::wstring Source = SourceAfter(Text, Prefix); !Source.empty())
                {
                    Hits.emplace_back(std::move(Source), Name);
                }
            }
        }

        int32_t Total = 0;
        for (const auto& [Source, Name] : Hits)
        {
            if (std::find(Guns.begin(), Guns.end(), Source) != Guns.end())
            {
                Total += Manager->GetStatValue(Name, Period);
            }
        }
        return Total;
    }

    // 4.0's weapon stats (FAthenaWeaponStats), one for each weapon the StatManager has stats for. 4.0's weapons add each
    // stat under every one of their ActualAnalyticFNames, which are their AnalyticTags ("weapon.ranged") and
    // "Tag:Weapon:<item definition, lower case>", so a weapon's stats are the "<Stat>:Tag:Weapon:<id>" ones: AFortPawn's
    // damage handler's "Damage.Dealt.Zone.<Normal|Critical>.To<Hostile|Friend>" and "Hit.Zone.<Normal|Critical>.To<Hostile|
    // Friend>", a building's "Damage.Dealt.Zone.Normal.ToStructure" and "Hit.Zone.Normal.ToStructure", and a weapon's fire
    // "Ranged.Bullets.Fired". Nothing records a weapon's eliminations or downs (the match's are in the player state).
    // The screen's accuracy is the hits over the shots: only the guns' hits count, as CountRangedHits does.
    void AddWeaponStats(FAthenaMatchStats& Stats, UStatManager* Manager, EStatRecordingPeriod Period)
    {
        static const std::pair<std::wstring, EAthenaWeaponStat> WeaponStatNames[] = {
            { L"Ranged.Bullets.Fired", EAthenaWeaponStat::RangedShots },
            { L"Damage.Dealt.Zone.Normal.ToHostile", EAthenaWeaponStat::NormalDamageToHostiles },
            { L"Damage.Dealt.Zone.Normal.ToFriend", EAthenaWeaponStat::NormalDamageToFriends },
            { L"Damage.Dealt.Zone.Normal.ToStructure", EAthenaWeaponStat::NormalDamageToStructures },
            { L"Damage.Dealt.Zone.Critical.ToHostile", EAthenaWeaponStat::CriticalDamageToHostiles },
            { L"Damage.Dealt.Zone.Critical.ToFriend", EAthenaWeaponStat::CriticalDamageToFriends },
            { L"Hit.Zone.Normal.ToHostile", EAthenaWeaponStat::NormalHitsToHostiles },
            { L"Hit.Zone.Normal.ToFriend", EAthenaWeaponStat::NormalHitsToFriends },
            { L"Hit.Zone.Normal.ToStructure", EAthenaWeaponStat::NormalHitsToStructures },
            { L"Hit.Zone.Critical.ToHostile", EAthenaWeaponStat::CriticalHitsToHostiles },
            { L"Hit.Zone.Critical.ToFriend", EAthenaWeaponStat::CriticalHitsToFriends },
        };
        static const std::wstring WeaponSource = L":Tag:Weapon:";

        std::vector<std::pair<std::wstring, std::vector<int32_t>>> Weapons;
        const TArray<UStat*>& AllStats = Manager->Stats;
        for (UStat* Each : AllStats)
        {
            if (!Each)
            {
                continue;
            }
            const FName Name = static_cast<FName>(Each->StatName);
            const std::wstring Text = Name.ToStdString();
            for (const auto& [StatName, Stat] : WeaponStatNames)
            {
                const size_t Prefix = StatName.size() + WeaponSource.size();
                // Names compare without case, as FNames do.
                if (Text.size() <= Prefix || _wcsnicmp(Text.c_str(), StatName.c_str(), StatName.size()) != 0
                    || _wcsnicmp(Text.c_str() + StatName.size(), WeaponSource.c_str(), WeaponSource.size()) != 0)
                {
                    continue;
                }
                std::wstring WeaponId = Text.substr(Prefix);
                std::transform(WeaponId.begin(), WeaponId.end(), WeaponId.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
                auto Weapon = std::find_if(Weapons.begin(), Weapons.end(), [&WeaponId](const auto& Entry) { return Entry.first == WeaponId; });
                if (Weapon == Weapons.end())
                {
                    Weapons.emplace_back(std::move(WeaponId), std::vector<int32_t>(static_cast<size_t>(EAthenaWeaponStat::Count), 0));
                    Weapon = Weapons.end() - 1;
                }
                Weapon->second[static_cast<size_t>(Stat)] += Manager->GetStatValue(Name, Period);
                break;
            }
        }

        for (auto& [WeaponId, Values] : Weapons)
        {
            if (Values[static_cast<size_t>(EAthenaWeaponStat::RangedShots)] <= 0)
            {
                for (EAthenaWeaponStat Hits : { EAthenaWeaponStat::NormalHitsToHostiles, EAthenaWeaponStat::NormalHitsToFriends, EAthenaWeaponStat::NormalHitsToStructures,
                         EAthenaWeaponStat::CriticalHitsToHostiles, EAthenaWeaponStat::CriticalHitsToFriends })
                {
                    Values[static_cast<size_t>(Hits)] = 0;
                }
            }
            FAthenaWeaponStats& Weapon = Stats.GetMember_WeaponStats().AddDefaulted();
            Weapon.WeaponId = FString(std::wstring_view(WeaponId));
            for (int32_t Index = 0; Index < static_cast<int32_t>(EAthenaWeaponStat::Count); ++Index)
            {
                Weapon.SetStat(static_cast<EAthenaWeaponStat>(Index), Values[static_cast<size_t>(Index)]);
            }
        }
    }

    // The objects of Json's array Name, each as its own text: the backend's are flat ({"source":1,"amount":10}).
    std::vector<std::string> JsonObjects(const std::string& Json, const std::string& Name)
    {
        std::vector<std::string> Objects;
        const size_t Key = Json.find("\"" + Name + "\"");
        size_t At = Key == std::string::npos ? std::string::npos : Json.find('[', Key);
        const size_t End = At == std::string::npos ? std::string::npos : Json.find(']', At);
        while (At != std::string::npos && End != std::string::npos)
        {
            const size_t Open = Json.find('{', At);
            const size_t Close = Open == std::string::npos ? std::string::npos : Json.find('}', Open);
            if (Open == std::string::npos || Close == std::string::npos || Close > End)
            {
                break;
            }
            Objects.push_back(Json.substr(Open, Close - Open + 1));
            At = Close + 1;
        }
        return Objects;
    }
}

void AFortPlayerController::SendMatchStatsForPlayer()
{
#if WITH_SERVER_CODE
    AFortPlayerControllerAthena* AthenaController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this));
    AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    if (!AthenaController || !AthenaState || Role != ROLE_Authority || !AFortPlayerControllerAthena::FindStaticFunction_ClientSendMatchStatsForPlayer()
        || !AFortPlayerControllerAthena::FindStaticFunction_ClientSendEndBattleRoyaleMatchForPlayer() || !FAthenaMatchStats::IsReflected() || !UAthenaPlayerMatchReport::HasStats())
    {
        return;
    }
    UAthenaPlayerMatchReport* Report = AthenaController->GetMatchReport();
    if (!Report || Report->HasStatsSent())
    {
        return;
    }

    UWorld* World = GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const int32_t Place = std::max<int32_t>(AthenaState->Place, 0);
    const int32_t TotalPlayers = std::max(GameState && AFortGameStateAthena::HasTotalPlayers() ? static_cast<int32_t>(GameState->TotalPlayers) : 0, Place);

    FAthenaMatchStats& Stats = Report->GetMatchStats();
    Stats.SetStat(FAthenaMatchStats::StatPlace(), Place);
    Stats.SetStat(FAthenaMatchStats::StatTotalPlayers(), TotalPlayers);
    Stats.SetStat(FAthenaMatchStats::StatSecondsAlive(), std::max<int32_t>(AthenaState->SecondsAlive, 0));
    Stats.SetStat(FAthenaMatchStats::StatKills(), std::max(AthenaState->GetKills(), 0));
    Stats.SetStat(FAthenaMatchStats::StatDowns(), AFortPlayerStateAthena::HasDownScore() ? std::max<int32_t>(AthenaState->DownScore, 0) : 0);
    Stats.SetStat(FAthenaMatchStats::StatAssists(), CountAssists(AthenaState));
    // The materials a player harvests count in the player state's stats (AFortPlayerStateZone::ModifyStat with the
    // resource's AccumulatingStatType); the StatManager's WoodAcquired ... MetalAcquired are never added to.
    const int32_t GatheredWood = AthenaState->GetStatValue(EFortReplicatedStat::GetWoodGathered(), false);
    const int32_t GatheredStone = AthenaState->GetStatValue(EFortReplicatedStat::GetStoneGathered(), false);
    const int32_t GatheredMetal = AthenaState->GetStatValue(EFortReplicatedStat::GetMetalGathered(), false);
    Stats.SetStat(FAthenaMatchStats::StatGatheredWood(), GatheredWood);
    Stats.SetStat(FAthenaMatchStats::StatGatheredStone(), GatheredStone);
    Stats.SetStat(FAthenaMatchStats::StatGatheredMetal(), GatheredMetal);
    Stats.SetStat(FAthenaMatchStats::StatMaterialsGathered(), GatheredWood + GatheredStone + GatheredMetal);

    UStatManager* Manager = StatManager;
    if (Manager)
    {
        const EStatRecordingPeriod Match = EStatRecordingPeriod::GetMap();
        // A build records one of the two: 1.10 "Ranged.Fire", 1.11 on "Ranged.Bullets.Fired" (the cartridges times
        // the bullets in each, 2.1 0x2A35D0).
        const int32_t ShotsFired = Manager->GetStatValueForAllSources(L"Ranged.Bullets.Fired", Match) + Manager->GetStatValueForAllSources(L"Ranged.Fire", Match);
        // Every hit on a player counts in both, a hit on a bot only in the zone stats (CountRangedHits).
        const int32_t Hits = std::max(Manager->GetStatValueForAllSources(L"Ranged.Hit.Hostile", Match), CountRangedHits(Manager, Match));
        const int32_t Headshots = Manager->GetStatValue(FName(L"Headshots"), Match);
        const int32_t UsedWood = Manager->GetStatValue(FName(L"WoodExpended"), Match);
        const int32_t UsedStone = Manager->GetStatValue(FName(L"StoneExpended"), Match);
        const int32_t UsedMetal = Manager->GetStatValue(FName(L"MetalExpended"), Match);
        Stats.SetStat(FAthenaMatchStats::StatRevives(), Manager->GetStatValue(FName(L"TeammateRevives"), Match));
        Stats.SetStat(FAthenaMatchStats::StatDamageDealtToHostiles(), Manager->GetStatValueForAllSources(L"Damage.Dealt.ToHostile", Match));
        Stats.SetStat(FAthenaMatchStats::StatDamageDealtToFriends(), Manager->GetStatValueForAllSources(L"Damage.Dealt.ToFriend", Match));
        Stats.SetStat(FAthenaMatchStats::StatDamageDealtToStructures(), Manager->GetStatValueForAllSources(L"Damage.Dealt.ToStructure", Match));
        Stats.SetStat(FAthenaMatchStats::StatDamageTaken(), Manager->GetStatValue(FName(L"DamageReceived"), Match));
        Stats.SetStat(FAthenaMatchStats::StatRangedShots(), ShotsFired);
        Stats.SetStat(FAthenaMatchStats::StatRangedHit(), Hits);
        Stats.SetStat(FAthenaMatchStats::StatRangedMiss(), std::max(ShotsFired - Hits, 0));
        Stats.SetStat(FAthenaMatchStats::StatAccuracy(), ShotsFired > 0 ? std::clamp(static_cast<float>(Hits) / static_cast<float>(ShotsFired), 0.0f, 1.0f) : 0.0f);
        Stats.SetStat(FAthenaMatchStats::StatTravelDistanceGround(), Manager->GetStatValue(FName(L"Travel.Distance.Ground"), Match));
        Stats.SetStat(FAthenaMatchStats::StatUsedWood(), UsedWood);
        Stats.SetStat(FAthenaMatchStats::StatUsedStone(), UsedStone);
        Stats.SetStat(FAthenaMatchStats::StatUsedMetal(), UsedMetal);
        Stats.SetStat(FAthenaMatchStats::StatMaterialsUsed(), UsedWood + UsedStone + UsedMetal);
        Stats.SetStat(FAthenaMatchStats::StatCriticalShots(), Headshots);
        Stats.SetStat(FAthenaMatchStats::StatCriticalHitsToHostiles(), Headshots);
    }
    else
    {
        UE_LOG(LogFortMatchStats, Warning, TEXT("SendMatchStatsForPlayer: %s has no StatManager, only its place, time alive, kills, downs, assists and materials gathered are sent"), *GetName());
    }

    // 4.0: the same stats in their slots, and the damage, hits and shots by weapon.
    const bool bStatSlots = FAthenaMatchStats::HasStatSlots();
    if (bStatSlots)
    {
        const int32_t Kills = std::max(AthenaState->GetKills(), 0);
        Stats.SetStat(EAthenaMatchStat::Placement, Place);
        Stats.SetStat(EAthenaMatchStat::SecondsAlive, std::max<int32_t>(AthenaState->SecondsAlive, 0));
        Stats.SetStat(EAthenaMatchStat::PersonalElimination, Kills);
        Stats.SetStat(EAthenaMatchStat::TeamEliminations, AFortPlayerStateAthena::HasTeamKillScore() ? std::max<int32_t>(AthenaState->TeamKillScore, Kills) : Kills);
        Stats.SetStat(EAthenaMatchStat::Downs, AFortPlayerStateAthena::HasDownScore() ? std::max<int32_t>(AthenaState->DownScore, 0) : 0);
        Stats.SetStat(EAthenaMatchStat::Assists, CountAssists(AthenaState));
        Stats.SetStat(EAthenaMatchStat::GatheredWood, GatheredWood);
        Stats.SetStat(EAthenaMatchStat::GatheredStone, GatheredStone);
        Stats.SetStat(EAthenaMatchStat::GatheredMetal, GatheredMetal);
        // One match played, won when the player's team placed first (UAthenaStatsSet's CumulativeStats add them up).
        Stats.SetStat(EAthenaMatchStat::Wins, Place == 1 ? 1 : 0);
        Stats.SetStat(EAthenaMatchStat::GamesPlayed, 1);
        if (Manager)
        {
            const EStatRecordingPeriod Match = EStatRecordingPeriod::GetMap();
            Stats.SetStat(EAthenaMatchStat::Revives, Manager->GetStatValue(FName(L"TeammateRevives"), Match));
            Stats.SetStat(EAthenaMatchStat::DamageTaken, Manager->GetStatValue(FName(L"DamageReceived"), Match));
            Stats.SetStat(EAthenaMatchStat::TravelDistanceGround, Manager->GetStatValue(FName(L"Travel.Distance.Ground"), Match));
            Stats.SetStat(EAthenaMatchStat::UsedWood, Manager->GetStatValue(FName(L"WoodExpended"), Match));
            Stats.SetStat(EAthenaMatchStat::UsedStone, Manager->GetStatValue(FName(L"StoneExpended"), Match));
            Stats.SetStat(EAthenaMatchStat::UsedMetal, Manager->GetStatValue(FName(L"MetalExpended"), Match));
            AddWeaponStats(Stats, Manager, Match);
        }
    }

    Report->SetStatsSent();
    AthenaController->ClientSendMatchStatsForPlayer(Stats);
    if (UAthenaPlayerMatchReport::HasTeamStats() && AFortPlayerControllerAthena::FindStaticFunction_ClientSendTeamStatsForPlayer())
    {
        FAthenaMatchTeamStats& TeamStats = Report->TeamStats;
        TeamStats.Place = Place;
        TeamStats.TotalPlayers = TotalPlayers;
        Report->bHasTeamStats = true;
        AthenaController->ClientSendTeamStatsForPlayer(TeamStats);
    }
    // The rewards follow with the backend's answer to the match's results (SendEndBattleRoyaleMatchForPlayer).

    if (bStatSlots)
    {
        int32_t Weapon[static_cast<int32_t>(EAthenaWeaponStat::Count)] = {};
        TStructArray<FAthenaWeaponStats>& Weapons = Stats.GetMember_WeaponStats();
        for (int32_t Index = 0; Index < Weapons.Num(); ++Index)
        {
            for (int32_t Stat = 0; Stat < static_cast<int32_t>(EAthenaWeaponStat::Count); ++Stat)
            {
                Weapon[Stat] += Weapons[Index].GetStat(static_cast<EAthenaWeaponStat>(Stat));
            }
        }
        const auto Of = [&Weapon](EAthenaWeaponStat Stat) { return Weapon[static_cast<int32_t>(Stat)]; };
        UE_LOG(LogFortMatchStats, Log, TEXT("SendMatchStatsForPlayer: %s placed %d of %d, %d s alive, %d kills, %d downs, %d assists, %d revives, %d weapons: damage %d to players, %d to friends, %d to structures, %d taken, %d shots, %d hits, %d critical, %d cm on the ground, materials %d gathered and %d used"),
            *GetName(), Place, TotalPlayers, Stats.GetStat(EAthenaMatchStat::SecondsAlive), Stats.GetStat(EAthenaMatchStat::PersonalElimination), Stats.GetStat(EAthenaMatchStat::Downs),
            Stats.GetStat(EAthenaMatchStat::Assists), Stats.GetStat(EAthenaMatchStat::Revives), Weapons.Num(),
            Of(EAthenaWeaponStat::NormalDamageToHostiles) + Of(EAthenaWeaponStat::CriticalDamageToHostiles), Of(EAthenaWeaponStat::NormalDamageToFriends) + Of(EAthenaWeaponStat::CriticalDamageToFriends),
            Of(EAthenaWeaponStat::NormalDamageToStructures), Stats.GetStat(EAthenaMatchStat::DamageTaken), Of(EAthenaWeaponStat::RangedShots),
            Of(EAthenaWeaponStat::NormalHitsToHostiles) + Of(EAthenaWeaponStat::CriticalHitsToHostiles), Of(EAthenaWeaponStat::CriticalHitsToHostiles),
            Stats.GetStat(EAthenaMatchStat::TravelDistanceGround), GatheredWood + GatheredStone + GatheredMetal,
            Stats.GetStat(EAthenaMatchStat::UsedWood) + Stats.GetStat(EAthenaMatchStat::UsedStone) + Stats.GetStat(EAthenaMatchStat::UsedMetal));
        return;
    }

    const auto Value = [&Stats](Reflection::FMemberInfo& Member) { return Member.IsAvailable() ? Reflection::AccessMember<int32_t>(&Stats, Member) : 0; };
    UE_LOG(LogFortMatchStats, Log, TEXT("SendMatchStatsForPlayer: %s placed %d of %d, %d s alive, %d kills, %d downs, %d assists, %d revives, damage %d to players, %d to friends, %d to structures, %d taken, %d shots, %d hits, %d misses, %d critical, %d cm on the ground, materials %d gathered and %d used"),
        *GetName(), Place, TotalPlayers, Value(FAthenaMatchStats::StatSecondsAlive()), Value(FAthenaMatchStats::StatKills()), Value(FAthenaMatchStats::StatDowns()),
        Value(FAthenaMatchStats::StatAssists()), Value(FAthenaMatchStats::StatRevives()), Value(FAthenaMatchStats::StatDamageDealtToHostiles()),
        Value(FAthenaMatchStats::StatDamageDealtToFriends()), Value(FAthenaMatchStats::StatDamageDealtToStructures()), Value(FAthenaMatchStats::StatDamageTaken()),
        Value(FAthenaMatchStats::StatRangedShots()), Value(FAthenaMatchStats::StatRangedHit()), Value(FAthenaMatchStats::StatRangedMiss()),
        std::max(Value(FAthenaMatchStats::StatCriticalShots()), Value(FAthenaMatchStats::StatCriticalHitsToHostiles())),
        Value(FAthenaMatchStats::StatTravelDistanceGround()), Value(FAthenaMatchStats::StatMaterialsGathered()), Value(FAthenaMatchStats::StatMaterialsUsed()));
#endif
}

void AFortPlayerController::SendEndBattleRoyaleMatchForPlayer(const std::string& RewardResultJson)
{
#if WITH_SERVER_CODE
    // The rewards came from MCP's answer to EndBattleRoyaleGame, the command Epic's server sent each player's athena
    // profile with the match's minutes alive, kills and placement; Core's backend gives the match's XP when the server
    // reports its results (battleroyale/recordresults) and answers what it gave each player, in FAthenaRewardResult's
    // form. Without the rewards the screen read "Total XP 0" and its XP bar stood still (#192). A player the backend gave
    // nothing (no answer, no athena profile) still gets bSuccess and no rewards, which end the screen's wait.
    AFortPlayerControllerAthena* AthenaController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this));
    if (!AthenaController || Role != ROLE_Authority || !AFortPlayerControllerAthena::FindStaticFunction_ClientSendEndBattleRoyaleMatchForPlayer())
    {
        return;
    }
    UScriptStruct* ResultStruct = FAthenaRewardResult::IsReflected() && !RewardResultJson.empty() ? FAthenaRewardResult::StaticStruct() : nullptr;
    if (!ResultStruct)
    {
        AthenaController->ClientSendEndBattleRoyaleMatchForPlayer(true, nullptr);
        return;
    }

    std::vector<uint8_t> ResultStorage(ResultStruct->GetStructureSize(), 0);
    FAthenaRewardResult* Result = reinterpret_cast<FAthenaRewardResult*>(ResultStorage.data());
    ResultStruct->InitializeStruct(Result);
    const auto Int = [](const std::string& Json, const char* Name) { return std::atoi(BackendRequest::JsonField(Json, Name).c_str()); };
    Result->SetReward(FAthenaRewardResult::RewardLevelsGained(), Int(RewardResultJson, "levelsGained"));
    Result->SetReward(FAthenaRewardResult::RewardBookLevelsGained(), Int(RewardResultJson, "bookLevelsGained"));
    Result->SetReward(FAthenaRewardResult::RewardTotalSeasonXpGained(), Int(RewardResultJson, "totalSeasonXpGained"));
    Result->SetReward(FAthenaRewardResult::RewardTotalBookXpGained(), Int(RewardResultJson, "totalBookXpGained"));
    if (FAthenaRewardResult::HasXpMultipliers())
    {
        for (const std::string& Group : JsonObjects(RewardResultJson, "xpMultipliers"))
        {
            FAthenaMatchXpMultiplierGroup& Multiplier = Result->GetMember_XpMultipliers().AddDefaulted();
            Multiplier.Source = static_cast<uint8_t>(Int(Group, "source"));
            Multiplier.Amount = Int(Group, "amount");
        }
    }
    // 1.10's groups hold MCP loot only, which the backend doesn't answer: its XP isn't listed.
    if (FAthenaRewardResult::HasRewards() && FAthenaAwardGroup::HasScore())
    {
        for (const std::string& Group : JsonObjects(RewardResultJson, "rewards"))
        {
            FAthenaAwardGroup& Award = Result->GetMember_Rewards().AddDefaulted();
            Award.RewardSource = static_cast<uint8_t>(Int(Group, "rewardSource"));
            Award.Score = Int(Group, "score");
            Award.SeasonXp = static_cast<float>(Int(Group, "seasonXp"));
            if (FAthenaAwardGroup::HasBookXp())
            {
                Award.BookXp = Int(Group, "bookXp");
            }
        }
    }
    AthenaController->ClientSendEndBattleRoyaleMatchForPlayer(true, Result);
    UE_LOG(LogFortMatchStats, Log, TEXT("SendEndBattleRoyaleMatchForPlayer: %s gained %d season XP, %d levels, %d Battle Stars, %d tiers"), *GetName(),
        Int(RewardResultJson, "totalSeasonXpGained"), Int(RewardResultJson, "levelsGained"), Int(RewardResultJson, "totalBookXpGained"), Int(RewardResultJson, "bookLevelsGained"));
    ResultStruct->DestroyStruct(Result);
#endif
}

void AFortPlayerController::ServerModifyStat_Implementation(FName StatName, int32_t Amount, uint8_t ModType, bool bForceStatSave)
{
#if WITH_SERVER_CODE
    // The RPC's exec thunk ends in the implementation's slot (2.1 0x1C0, after ServerModifyStat_Validate's 0x1BF).
    static const int32_t Index = Finder::ImplementationIndex(FindStaticFunction_ServerModifyStat());
    if (Index < 0 || Role != ROLE_Authority)
    {
        return;
    }
    CallVirtual<void>(this, Index, StatName, Amount, ModType, bForceStatSave);
#endif
}
