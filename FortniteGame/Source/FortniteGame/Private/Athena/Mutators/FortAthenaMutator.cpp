#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Finder.h"
#include "FortTeamInfo.h"
#include "GameFramework/PlayerController.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Player/FortPlayerController.h"

bool AFortAthenaMutator::HasOnPostEarnedKill()
{
#if WITH_SERVER_CODE
    UClass* const PlayerControllerClass = APlayerController::StaticClass();
    return StaticClass() && AFortGameStateAthena::HasMutatorGenericInts() && PlayerControllerClass
        && Finder::ResolveOnPostEarnedKillLive(AFortGameStateAthena::MutatorGenericInt0Offset(), AFortGameStateAthena::MutatorGenericInt1Offset(),
            PlayerControllerClass->GetPropertiesSize());
#else
    return false;
#endif
}

void AFortAthenaMutator::FortGameStateAthena_OnPostEarnedKill(AFortPlayerController* KillerController)
{
#if WITH_SERVER_CODE
    if (HasOnPostEarnedKill() && KillerController)
    {
        CallVirtual<void>(this, Natives::AFortAthenaMutator_OnPostEarnedKillIndex, KillerController);
    }
#endif
}

void AFortAthenaMutator_TDM::ScoreBotKill(AFortGameStateAthena* GameState, AFortPlayerStateAthena* KillerPlayerState)
{
#if WITH_SERVER_CODE
    // As 0xD3C1B0: the earner's team's team info, if it is an AFortTeamInfoAthena; its score one more; with
    // bIsTwoTeamTDM, the score into the game state's int for that team.
    AFortTeamInfo* TeamInfo = KillerPlayerState && GameState ? static_cast<AFortTeamInfo*>(KillerPlayerState->PlayerTeam) : nullptr;
    UClass* const TeamInfoAthenaClass = AFortTeamInfoAthena::StaticClass();
    if (!TeamInfo || !TeamInfoAthenaClass || !reinterpret_cast<UObject*>(TeamInfo)->IsA(TeamInfoAthenaClass) || Natives::AFortTeamInfoAthena_CurrentTeamScore < 0)
    {
        return;
    }
    int32_t& CurrentTeamScore = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(TeamInfo) + Natives::AFortTeamInfoAthena_CurrentTeamScore);
    ++CurrentTeamScore;
    if (!bIsTwoTeamTDM)
    {
        return;
    }
    if (static_cast<uint8_t>(KillerPlayerState->TeamIndex) == EFortTeam::GetHumanPvP_Team1())
    {
        GameState->MutatorGenericInt_0 = CurrentTeamScore;
    }
    else
    {
        GameState->MutatorGenericInt_1 = CurrentTeamScore;
    }
#endif
}
