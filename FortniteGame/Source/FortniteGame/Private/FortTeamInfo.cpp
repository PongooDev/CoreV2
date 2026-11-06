#include "pch.h"
#include "FortTeamInfo.h"
#include "Building/BuildingActor.h"
#include "Finder.h"
#include "Natives.h"
#include "Player/FortPlayerState.h"
#include "Pawns/FortPawn.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/ScriptDelegates.h"

bool AFortTeamPrivateInfo::SetHealthAndShield(AFortPlayerStateZone* PlayerState)
{
#if WITH_SERVER_CODE
    if (!PlayerState || !AFortPlayerState::HasPlayerTeamPrivate() || !AFortPlayerStateZone::HasHealthAndShield())
    {
        return false;
    }
    const uint8_t* const State = reinterpret_cast<const uint8_t*>(PlayerState);
    if (!Finder::ResolveTeamPrivateHealthAndShieldLive(reinterpret_cast<const uint8_t*>(&PlayerState->PlayerTeamPrivate) - State,
            reinterpret_cast<const uint8_t*>(&PlayerState->MaxShield) - State))
    {
        return false;
    }
    Natives::AFortTeamPrivateInfo_SetHealthAndShield(this, PlayerState);
    return true;
#else
    return false;
#endif
}

void AFortTeamInfo::AddSpottedActor(AFortPlayerController* Spotter, AActor* SpottedActor)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !SpottedActor)
    {
        return;
    }

    FTeamSpottedActorInfoArray& SpottedActors = TeamSpottedActors;
    TStructArray<FTeamSpottedActorInfo>& Infos = SpottedActors.SpottedActorInfo;
    for (int32_t Index = 0; Index < Infos.Num(); ++Index)
    {
        if (static_cast<AActor*>(Infos[Index].SpottedActor) == SpottedActor)
        {
            return;
        }
    }

    FTeamSpottedActorInfo& Info = Infos.AddDefaulted();
    Info.Spotter = Spotter;
    Info.SpottedActor = SpottedActor;
    SpottedActors.MarkItemDirty(Info);

    const FScriptDelegate OnDiedDelegate(this, StaticFunction_OnSpottedActorDied()->GetFName());
    if (AFortPawn* SpottedPawn = Cast<AFortPawn>(SpottedActor))
    {
        FMulticastScriptDelegate& OnDied = SpottedPawn->OnDied;
        OnDied.AddUnique(OnDiedDelegate);
    }
    else if (ABuildingActor* SpottedBuilding = Cast<ABuildingActor>(SpottedActor))
    {
        FMulticastScriptDelegate& OnDied = SpottedBuilding->OnDied;
        OnDied.AddUnique(OnDiedDelegate);
    }
#endif
}
