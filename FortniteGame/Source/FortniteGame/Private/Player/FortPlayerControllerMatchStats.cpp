#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "AthenaPlayerMatchReport.h"
#include "BackendRequest.h"
#include "Engine/ActorChannel.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Natives.h"
#include "UObject/WeakObjectPtr.h"
#include "GameFramework/PlayerState.h"
#include "Logging/LogMacros.h"
#include "ObjectLayout.h"
#include "StatManager.h"
#include "Templates/Casts.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <unordered_map>
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
// The screen fills its stats grid whenever the stats come (AthenaMatchStatsPartial's OnStatsAvailable, bound to the
// report's delegate and called again when the screen is made) and shows "Team Still Alive" (WaitingForMatchToFinish)
// until the rewards come, the place (OnTeamStatsAvailable) apart from both. So from 1.11 a player's stats go as soon as
// they are out of the match (AFortPlayerControllerAthena::OnPawnDied), their team still in or not, and the team's
// place and the rewards once the team has a place: a player out of a squad, a 50v50 side or a team filled with bots
// read 0 in every stat under "Team Still Alive" while the team played on (#403). 1.10's stats hold the place, so they
// still wait for it.

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

    // The player's own stats for their end-of-match screen (AFortPlayerController::SendMatchStatsForPlayer sends them
    // once, the report's bHasStats): the match's values (period Map) in the server's report, sent with
    // ClientSendMatchStatsForPlayer. Place is 0 while the player's team is still in.
    void SendPlayerMatchStats(AFortPlayerController* Controller, AFortPlayerControllerAthena* AthenaController, AFortPlayerStateAthena* AthenaState, UAthenaPlayerMatchReport* Report,
        UStatManager* Manager, int32_t Place, int32_t TotalPlayers)
    {
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
            UE_LOG(LogFortMatchStats, Warning, TEXT("SendMatchStatsForPlayer: %s has no StatManager, only its place, time alive, kills, downs, assists and materials gathered are sent"), *Controller->GetName());
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
            UE_LOG(LogFortMatchStats, Log, TEXT("SendMatchStatsForPlayer: %s, place %d of %d, %d s alive, %d kills, %d downs, %d assists, %d revives, %d weapons: damage %d to players, %d to friends, %d to structures, %d taken, %d shots, %d hits, %d critical, %d cm on the ground, materials %d gathered and %d used"),
                *Controller->GetName(), Place, TotalPlayers, Stats.GetStat(EAthenaMatchStat::SecondsAlive), Stats.GetStat(EAthenaMatchStat::PersonalElimination), Stats.GetStat(EAthenaMatchStat::Downs),
                Stats.GetStat(EAthenaMatchStat::Assists), Stats.GetStat(EAthenaMatchStat::Revives), Weapons.Num(),
                Of(EAthenaWeaponStat::NormalDamageToHostiles) + Of(EAthenaWeaponStat::CriticalDamageToHostiles), Of(EAthenaWeaponStat::NormalDamageToFriends) + Of(EAthenaWeaponStat::CriticalDamageToFriends),
                Of(EAthenaWeaponStat::NormalDamageToStructures), Stats.GetStat(EAthenaMatchStat::DamageTaken), Of(EAthenaWeaponStat::RangedShots),
                Of(EAthenaWeaponStat::NormalHitsToHostiles) + Of(EAthenaWeaponStat::CriticalHitsToHostiles), Of(EAthenaWeaponStat::CriticalHitsToHostiles),
                Stats.GetStat(EAthenaMatchStat::TravelDistanceGround), GatheredWood + GatheredStone + GatheredMetal,
                Stats.GetStat(EAthenaMatchStat::UsedWood) + Stats.GetStat(EAthenaMatchStat::UsedStone) + Stats.GetStat(EAthenaMatchStat::UsedMetal));
            return;
        }

        const auto Value = [&Stats](Reflection::FMemberInfo& Member) { return Member.IsAvailable() ? Reflection::AccessMember<int32_t>(&Stats, Member) : 0; };
        UE_LOG(LogFortMatchStats, Log, TEXT("SendMatchStatsForPlayer: %s, place %d of %d, %d s alive, %d kills, %d downs, %d assists, %d revives, damage %d to players, %d to friends, %d to structures, %d taken, %d shots, %d hits, %d misses, %d critical, %d cm on the ground, materials %d gathered and %d used"),
            *Controller->GetName(), Place, TotalPlayers, Value(FAthenaMatchStats::StatSecondsAlive()), Value(FAthenaMatchStats::StatKills()), Value(FAthenaMatchStats::StatDowns()),
            Value(FAthenaMatchStats::StatAssists()), Value(FAthenaMatchStats::StatRevives()), Value(FAthenaMatchStats::StatDamageDealtToHostiles()),
            Value(FAthenaMatchStats::StatDamageDealtToFriends()), Value(FAthenaMatchStats::StatDamageDealtToStructures()), Value(FAthenaMatchStats::StatDamageTaken()),
            Value(FAthenaMatchStats::StatRangedShots()), Value(FAthenaMatchStats::StatRangedHit()), Value(FAthenaMatchStats::StatRangedMiss()),
            std::max(Value(FAthenaMatchStats::StatCriticalShots()), Value(FAthenaMatchStats::StatCriticalHitsToHostiles())),
            Value(FAthenaMatchStats::StatTravelDistanceGround()), Value(FAthenaMatchStats::StatMaterialsGathered()), Value(FAthenaMatchStats::StatMaterialsUsed()));
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
    if (!Report)
    {
        return;
    }

    UWorld* World = GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const int32_t Place = std::max<int32_t>(AthenaState->Place, 0);
    const int32_t TotalPlayers = std::max(GameState && AFortGameStateAthena::HasTotalPlayers() ? static_cast<int32_t>(GameState->TotalPlayers) : 0, Place);

    // From 1.11 the team's place goes on its own, once there is one; 1.10's stats hold it and wait for it.
    const bool bTeamStatsApart = UAthenaPlayerMatchReport::HasTeamStats() && AFortPlayerControllerAthena::FindStaticFunction_ClientSendTeamStatsForPlayer();
    if (!Report->HasStatsSent() && (Place > 0 || bTeamStatsApart))
    {
        SendPlayerMatchStats(this, AthenaController, AthenaState, Report, StatManager, Place, TotalPlayers);
    }
    if (bTeamStatsApart && Place > 0 && !Report->bHasTeamStats)
    {
        FAthenaMatchTeamStats& TeamStats = Report->TeamStats;
        TeamStats.Place = Place;
        TeamStats.TotalPlayers = TotalPlayers;
        Report->bHasTeamStats = true;
        AthenaController->ClientSendTeamStatsForPlayer(TeamStats);
        UE_LOG(LogFortMatchStats, Log, TEXT("SendMatchStatsForPlayer: %s's team placed %d of %d"), *GetName(), Place, TotalPlayers);
    }
    // The rewards follow with the backend's answer to the match's results (SendEndBattleRoyaleMatchForPlayer).
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
    // 3.2+: the XP before the anti-addiction penalty, which the screen's boosts share (#286). Epic's MCP answers it; a
    // backend from before it takes no penalty, so it is the season XP gained.
    const std::string PrePenaltySeasonXpGained = BackendRequest::JsonField(RewardResultJson, "prePenaltySeasonXpGained");
    Result->SetReward(FAthenaRewardResult::RewardPrePenaltySeasonXpGained(),
        PrePenaltySeasonXpGained.empty() ? Int(RewardResultJson, "totalSeasonXpGained") : std::atoi(PrePenaltySeasonXpGained.c_str()));
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

void AFortPlayerController::ReplicateServerFinishedLoadingAfterGameState()
{
#if WITH_SERVER_CODE
    // The end-of-match XP bar starts from the client's report's InitialLevelInfo, which the client's
    // AFortPlayerControllerAthena::OnReadyToStartMatch (4.1 0xA68540) stores, and only if the report exists ("Match report
    // could not be found to store initial level info!"). The report is made in the controller's BeginPlay (0xA52700),
    // which a client calls only once the world has begun play there, when the game state has replicated
    // (OnRep_ReplicatedHasBegunPlay). OnReadyToStartMatch doesn't wait for that: the client's ready check (0xF9A900, on
    // the timer that ClientOnGenericPlayerInitialization starts) runs it once the client has finished loading, and
    // CheckFinishedLoading (0xFA77E0) finishes once bHasServerFinishedLoading has come, its building foundation wait
    // (0xF585D0) passing at once without a game state. When the game state came after bHasServerFinishedLoading, the
    // level was never stored: the screen read "LEVEL 0", MAX, with a full bar that never moved (#296, and #294 in LTMs).
    // A 4.0 client logged it too: "OnReadyToStartMatch did not have GameState", then the missing report, with the game
    // state 36 ms later. It comes late in matches with many actors to send first (bots' player states are each forced to
    // replicate when they join). The server's flag now goes out only once the client has the game state: its channel is
    // open on the player's connection and every reliable bunch of it (the actor's spawn) is acknowledged. The game state
    // is forced to replicate in the meantime, and a connection that has not received it after ten seconds gets the flag
    // anyway.
    struct FFinishedLoadingHold
    {
        TWeakObjectPtr<AFortPlayerController> Controller;
        std::chrono::steady_clock::time_point Started;
        bool bHeld = false;
        bool bReleased = false;
    };
    static std::unordered_map<const UNetConnection*, FFinishedLoadingHold> Holds;
    constexpr auto MaxHoldTime = std::chrono::seconds(10);

    if (!Member_bHasServerFinishedLoading.IsAvailable() || !UAthenaPlayerMatchReport::HasInitialLevelInfo() || !Natives::GWorld
        || Natives::UNetConnection_ActorChannels < 0 || Natives::UChannel_NumOutRec < 0)
    {
        return;
    }
    UWorld* World = static_cast<UWorld*>(GWorld);
    UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
    AActor* GameState = World ? Cast<AActor>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!Driver || !GameState)
    {
        return;
    }

    const auto Now = std::chrono::steady_clock::now();
    std::unordered_map<const UNetConnection*, FFinishedLoadingHold> Current;
    bool bAnyHeld = false;
    for (UNetConnection* Connection : static_cast<TArray<UNetConnection*>&>(Driver->ClientConnections))
    {
        AFortPlayerController* Controller = Connection && Connection->State == USOCK_Open ? Cast<AFortPlayerController>(static_cast<AActor*>(Connection->OwningActor)) : nullptr;
        if (!Controller)
        {
            continue;
        }
        const auto Found = Holds.find(Connection);
        FFinishedLoadingHold Hold = Found != Holds.end() && Found->second.Controller.Get() == Controller ? Found->second : FFinishedLoadingHold{ Controller, Now };
        if (!Hold.bReleased)
        {
            UActorChannel* Channel = Connection->ActorChannels.FindRef(GameState);
            const bool bHasGameState = Channel && !Channel->Closing && Channel->NumOutRec <= 0;
            if (bHasGameState || Now - Hold.Started >= MaxHoldTime)
            {
                Hold.bReleased = true;
                if (Hold.bHeld)
                {
                    Controller->bHasServerFinishedLoading = true;
                    const long long HeldFor = std::chrono::duration_cast<std::chrono::milliseconds>(Now - Hold.Started).count();
                    if (bHasGameState)
                    {
                        UE_LOG(LogFortMatchStats, Log, TEXT("%s has the game state: bHasServerFinishedLoading replicates (held %lld ms)"), *Controller->GetName(), HeldFor);
                    }
                    else
                    {
                        UE_LOG(LogFortMatchStats, Warning, TEXT("%s has not received the game state in %lld ms: bHasServerFinishedLoading replicates anyway"), *Controller->GetName(), HeldFor);
                    }
                }
            }
            else if (Controller->bHasServerFinishedLoading)
            {
                Controller->bHasServerFinishedLoading = false;
                if (!Hold.bHeld)
                {
                    UE_LOG(LogFortMatchStats, Log, TEXT("%s: bHasServerFinishedLoading waits for the game state to reach the client"), *Controller->GetName());
                }
                Hold.bHeld = true;
            }
            bAnyHeld |= !Hold.bReleased;
        }
        Current.emplace(Connection, Hold);
    }
    Holds = std::move(Current);
    if (bAnyHeld)
    {
        GameState->ForceNetUpdate();
    }
#endif
}
