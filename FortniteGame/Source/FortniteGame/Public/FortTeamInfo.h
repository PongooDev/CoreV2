#pragma once

#include "Engine/NetSerialization.h"
#include "GameFramework/Info.h"
#include "StructArray.h"

class AActor;
class AController;
class AFortPlayerController;
class AFortPlayerStateZone;

class EFortTeam
{
    DefineUnrealEnum(EFortTeam, FortniteGame)
    DefineUEnumValue(HumanCampaign)
    DefineUEnumValue(Monster)
    DefineUEnumValue(HumanPvP_Team1)
    DefineUEnumValue(HumanPvP_Team2)
    // 7.30 has Spectator 103 and MAX 104; 7.40 (CL-5046157) keeps only HumanPvP_Team1 and 2 named (a team is a
    // uint8 from HumanPvP_Team1 on), with Spectator 0 and MAX 255. Teams are counted from HumanPvP_Team1, and MAX is
    // read with FindValue where it is needed (255 is also its "not found").
    DefineUEnumValue(Spectator)
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

// The team's private, team-only replicated data (3.0 onwards): RepData, a fast array of per-player values (health and
// shield among them) that the team's clients read for their squadmates.
class AFortTeamPrivateInfo : public AInfo
{
    DefineUnrealClass(AFortTeamPrivateInfo, FortniteGame)

public:
    // The player state's CurrentHealth, MaxHealth, CurrentShield and MaxShield into RepData (3.5.1 0xF83CB0). False where
    // the build's native wasn't found.
    bool SetHealthAndShield(AFortPlayerStateZone* PlayerState);
    // Finds that native (Finder::ResolveTeamPrivateHealthAndShieldLive) from the player state's offsets, as the match's
    // world starts: Battle Royale's player bots resolve it then, so its search doesn't hold their first frame.
    static bool ResolveSetHealthAndShield();
    // From 8.30 AFortTeamInfo::AddToTeam and RemoveFromTeam skip a bot's player state here (bIsABot), so a player bot
    // gets and loses its RepData entries through these, the same calls they make for a player. False where the build
    // has no such gate (AddToTeam already gave the bot its entries) or the calls weren't found.
    bool AddPlayerState(AFortPlayerStateZone* PlayerState);
    bool RemovePlayerState(AFortPlayerStateZone* PlayerState);
    // Finds those calls (Finder::ResolveTeamPrivatePlayerStateLive) as the match's world starts.
    static bool ResolvePlayerStateEntries();
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
    // The team's AFortTeamPrivateInfo (8.50 +0x390).
    DefineUProperty(AFortTeamPrivateInfo*, PrivateInfo)
    static bool HasPrivateInfo() { return Member_PrivateInfo.IsAvailable(); }
    DefineUStructProperty(FTeamSpottedActorInfoArray, TeamSpottedActors)

    void AddSpottedActor(AFortPlayerController* Spotter, AActor* SpottedActor);
};

// Battle Royale's team. Its score is native (21.40's CurrentTeamScore, PrevTeamScore, PrevPlacement, bTeamIsDead,
// TeamPlacement; 6.31 +0x428 on): Natives::AFortTeamInfoAthena_CurrentTeamScore, where found.
class AFortTeamInfoAthena : public AFortTeamInfo
{
    DefineUnrealClass(AFortTeamInfoAthena, FortniteGame)
};
