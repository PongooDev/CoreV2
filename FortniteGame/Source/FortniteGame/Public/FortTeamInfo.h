#pragma once

#include "Engine/NetSerialization.h"
#include "GameFramework/Info.h"
#include "StructArray.h"

class AActor;
class AController;
class AFortPlayerController;

class EFortTeam
{
    DefineUnrealEnum(EFortTeam, FortniteGame)
    DefineUEnumValue(HumanCampaign)
    DefineUEnumValue(Monster)
    DefineUEnumValue(HumanPvP_Team1)
    DefineUEnumValue(HumanPvP_Team2)
    DefineUEnumValue(HumanPvP_Team3)
    DefineUEnumValue(HumanPvP_Team4)
    DefineUEnumValue(HumanPvP_Team5)
    DefineUEnumValue(HumanPvP_Team6)
    DefineUEnumValue(HumanPvP_Team7)
    DefineUEnumValue(HumanPvP_Team8)
    DefineUEnumValue(HumanPvP_Team9)
    DefineUEnumValue(HumanPvP_Team10)
    DefineUEnumValue(Spectator)
    DefineUEnumValue(MAX)
};

struct FTeamSpottedActorInfo : public FFastArraySerializerItem
{
    DefineUnrealStruct(FTeamSpottedActorInfo, FortniteGame)
    DefineUProperty(AFortPlayerController*, Spotter)
    DefineUProperty(AActor*, SpottedActor)
};

struct FTeamSpottedActorInfoArray : public FFastArraySerializer
{
    DefineUnrealStruct(FTeamSpottedActorInfoArray, FortniteGame)
    DefineUStructArrayProperty(FTeamSpottedActorInfo, SpottedActorInfo)
};

class AFortTeamInfo : public AInfo
{
    DefineUnrealClass(AFortTeamInfo, FortniteGame)
    DefineUFunction(OnSpottedActorDied)

public:
    // The team's controllers: AddToTeam (3.2 0xC01AE0) adds one, RemoveFromTeam takes it off.
    DefineUProperty(TArray<AController*>, TeamMembers)
    static bool HasTeamMembers() { return Member_TeamMembers.IsAvailable(); }
    DefineUProperty(EFortTeam, Team)
    DefineUStructProperty(FTeamSpottedActorInfoArray, TeamSpottedActors)

    void AddSpottedActor(AFortPlayerController* Spotter, AActor* SpottedActor);
};
