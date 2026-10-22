#pragma once

#include "Engine/NetSerialization.h"
#include "GameFramework/Info.h"
#include "StructArray.h"

class AActor;
class AFortPlayerController;

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
    DefineUStructProperty(FTeamSpottedActorInfoArray, TeamSpottedActors)

    void AddSpottedActor(AFortPlayerController* Spotter, AActor* SpottedActor);
};
