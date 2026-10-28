#pragma once

#include "UObject/Object.h"

class UFortRegisteredPlayerInfo;
struct FFortTeamMemberInfo;

class UFortUITeamInfo : public UObject
{
    DefineUnrealClass(UFortUITeamInfo, FortniteGame)
    DefineUFunction(GetTeamMembers)

public:
    bool GetTeamMemberInfo(UFortRegisteredPlayerInfo* PlayerInfo, FFortTeamMemberInfo& OutInfo);

    static void Hook();

private:
    static int32_t PickAthenaMenuHero();

    static inline bool (*GetTeamMemberInfoOG)(UFortUITeamInfo* This, UFortRegisteredPlayerInfo* PlayerInfo, FFortTeamMemberInfo& OutInfo) = nullptr;
    static bool GetTeamMemberInfoHK(UFortUITeamInfo* This, UFortRegisteredPlayerInfo* PlayerInfo, FFortTeamMemberInfo& OutInfo);
};
