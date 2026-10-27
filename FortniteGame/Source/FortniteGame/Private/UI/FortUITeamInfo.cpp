#include "pch.h"
#include "UI/FortUITeamInfo.h"
#include "Finder.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Quests/FortQuestManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace
{
    thread_local UFortRegisteredPlayerInfo* AthenaMenuHeroPlayer = nullptr;
}

bool UFortUITeamInfo::GetTeamMemberInfo(UFortRegisteredPlayerInfo* PlayerInfo, FFortTeamMemberInfo& OutInfo)
{
    // Every Battle Royale lobby showed each party member, the player's own pad included, as a different default character
    // (Core-Backend #58). 1.7.2 publishes no Battle Royale character to the party (FFortPartyMemberRepState has only the
    // Save the World hero) and has no athena profile: each client gives every registered player an AthenaMenuHeroDef of
    // its own, a random UFortGameData::DefaultAthenaHeroes entry. The pick's rand() is PickAthenaMenuHero, which draws
    // from the player's account id instead, so every client picks the same character for them.
    UFortHero* const PreviousHero = PlayerInfo ? PlayerInfo->AthenaMenuHeroDef : nullptr;
    AthenaMenuHeroPlayer = PlayerInfo;
    const bool bFilled = GetTeamMemberInfoOG(this, PlayerInfo, OutInfo);
    AthenaMenuHeroPlayer = nullptr;

    UFortHero* const Hero = PlayerInfo && !PreviousHero ? PlayerInfo->AthenaMenuHeroDef : nullptr;
    if (Hero)
    {
        const FUniqueNetIdRepl& PlayerId = PlayerInfo->PlayerID;
        UE_LOG(LogFort, Log, TEXT("GetTeamMemberInfo: %s shows %s in the Battle Royale lobby"), *PlayerId.ToString(), *GetNameSafe(Hero->GetHeroTypeBP()));
    }
    return bFilled;
}

int32_t UFortUITeamInfo::PickAthenaMenuHero()
{
    // Stands in for rand() (0 to RAND_MAX) in the pick: the index is the number of loaded DefaultAthenaHeroes times this
    // over RAND_MAX, the same list in the same order on every client.
    if (!AthenaMenuHeroPlayer)
    {
        return Natives::CRT_rand();
    }
    const FUniqueNetIdRepl& PlayerId = AthenaMenuHeroPlayer->PlayerID;
    if (!PlayerId.IsValid())
    {
        return Natives::CRT_rand();
    }

    const FString Id = PlayerId.ToString().ToLower();
    uint32_t Hash = 2166136261u;
    for (int32_t Index = 0; Index < Id.Len(); ++Index)
    {
        Hash = (Hash ^ static_cast<uint32_t>((*Id)[Index])) * 16777619u;
    }
    return static_cast<int32_t>((Hash ^ (Hash >> 15)) & 0x7FFF);
}

bool UFortUITeamInfo::GetTeamMemberInfoHK(UFortUITeamInfo* This, UFortRegisteredPlayerInfo* PlayerInfo, FFortTeamMemberInfo& OutInfo)
{
    return This->GetTeamMemberInfo(PlayerInfo, OutInfo);
}

void UFortUITeamInfo::Hook()
{
    if (!Finder::ResolveAthenaMenuHero())
    {
        return;
    }
    Hooks::RedirectCall(L"UFortUITeamInfo::GetTeamMemberInfo rand", Sites::TeamMemberInfoAthenaMenuHeroRand, reinterpret_cast<void*>(&PickAthenaMenuHero));
    Hooks::Detour(L"UFortUITeamInfo::GetTeamMemberInfo", reinterpret_cast<void*>(Natives::UFortUITeamInfo_GetTeamMemberInfo), &GetTeamMemberInfoHK, &GetTeamMemberInfoOG);
}
