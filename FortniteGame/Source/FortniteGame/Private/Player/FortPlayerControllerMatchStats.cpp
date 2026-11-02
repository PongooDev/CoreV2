#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "AthenaPlayerMatchReport.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/PlayerState.h"
#include "Logging/LogMacros.h"
#include "ObjectLayout.h"
#include "StatManager.h"
#include "Templates/Casts.h"

#include <algorithm>

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
    if (!Report || Report->bHasStats)
    {
        return;
    }

    UWorld* World = GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const int32_t Place = std::max<int32_t>(AthenaState->Place, 0);
    const int32_t TotalPlayers = std::max(GameState && AFortGameStateAthena::HasTotalPlayers() ? static_cast<int32_t>(GameState->TotalPlayers) : 0, Place);

    FAthenaMatchStats& Stats = Report->Stats;
    Stats.SetStat(FAthenaMatchStats::StatPlace(), Place);
    Stats.SetStat(FAthenaMatchStats::StatTotalPlayers(), TotalPlayers);
    Stats.SetStat(FAthenaMatchStats::StatSecondsAlive(), std::max<int32_t>(AthenaState->SecondsAlive, 0));
    Stats.SetStat(FAthenaMatchStats::StatKills(), std::max(AthenaState->GetKills(), 0));
    Stats.SetStat(FAthenaMatchStats::StatDowns(), AFortPlayerStateAthena::HasDownScore() ? std::max<int32_t>(AthenaState->DownScore, 0) : 0);
    Stats.SetStat(FAthenaMatchStats::StatAssists(), CountAssists(AthenaState));

    UStatManager* Manager = StatManager;
    if (Manager)
    {
        const EStatRecordingPeriod Match = EStatRecordingPeriod::GetMap();
        const int32_t ShotsFired = Manager->GetStatValueForAllSources(L"Ranged.Fire", Match);
        const int32_t Hits = Manager->GetStatValueForAllSources(L"Ranged.Hit.Hostile", Match);
        const int32_t Headshots = Manager->GetStatValue(FName(L"Headshots"), Match);
        const int32_t GatheredWood = Manager->GetStatValue(FName(L"WoodAcquired"), Match);
        const int32_t GatheredStone = Manager->GetStatValue(FName(L"StoneAcquired"), Match);
        const int32_t GatheredMetal = Manager->GetStatValue(FName(L"MetalAcquired"), Match);
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
        Stats.SetStat(FAthenaMatchStats::StatGatheredWood(), GatheredWood);
        Stats.SetStat(FAthenaMatchStats::StatGatheredStone(), GatheredStone);
        Stats.SetStat(FAthenaMatchStats::StatGatheredMetal(), GatheredMetal);
        Stats.SetStat(FAthenaMatchStats::StatMaterialsGathered(), GatheredWood + GatheredStone + GatheredMetal);
        Stats.SetStat(FAthenaMatchStats::StatUsedWood(), UsedWood);
        Stats.SetStat(FAthenaMatchStats::StatUsedStone(), UsedStone);
        Stats.SetStat(FAthenaMatchStats::StatUsedMetal(), UsedMetal);
        Stats.SetStat(FAthenaMatchStats::StatMaterialsUsed(), UsedWood + UsedStone + UsedMetal);
        Stats.SetStat(FAthenaMatchStats::StatCriticalShots(), Headshots);
        Stats.SetStat(FAthenaMatchStats::StatCriticalHitsToHostiles(), Headshots);
    }
    else
    {
        UE_LOG(LogFortMatchStats, Warning, TEXT("SendMatchStatsForPlayer: %s has no StatManager, only its place, time alive, kills, downs and assists are sent"), *GetName());
    }

    Report->bHasStats = true;
    AthenaController->ClientSendMatchStatsForPlayer(Stats);
    if (UAthenaPlayerMatchReport::HasTeamStats() && AFortPlayerControllerAthena::FindStaticFunction_ClientSendTeamStatsForPlayer())
    {
        FAthenaMatchTeamStats& TeamStats = Report->TeamStats;
        TeamStats.Place = Place;
        TeamStats.TotalPlayers = TotalPlayers;
        Report->bHasTeamStats = true;
        AthenaController->ClientSendTeamStatsForPlayer(TeamStats);
    }
    // The rewards came from MCP's answer to EndBattleRoyaleGame, which nothing sends any more: none, but bSuccess.
    AthenaController->ClientSendEndBattleRoyaleMatchForPlayer(true, nullptr);

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
