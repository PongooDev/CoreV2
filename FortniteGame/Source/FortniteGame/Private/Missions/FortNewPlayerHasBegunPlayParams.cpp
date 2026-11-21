#include "pch.h"
#include "Missions/FortNewPlayerHasBegunPlayParams.h"
#include "Logging/LogMacros.h"
#include "Missions/FortMissionLibrary.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortMission, Log, All);

UFortNewPlayerHasBegunPlayParams* UFortNewPlayerHasBegunPlayParams::SetParams(AFortPlayerController* NewPlayerPC)
{
    Reflection::FParams Params(FindStaticFunction_SetParams(), true);
    Params.Set(L"_NewPlayerPC", NewPlayerPC);
    return Params.Call(this) ? Params.Take<UFortNewPlayerHasBegunPlayParams*>(L"ThisParam") : nullptr;
}

void UFortNewPlayerHasBegunPlayParams::SendNewPlayerHasBegunPlay(AFortPlayerController* NewPlayerPC)
{
#if WITH_SERVER_CODE
    // Core's: the event's sender is stripped from the client builds (7.30 CL-4834550: nothing constructs this class, its
    // StaticClass 0x1853D80 is referenced only by its registration) and 21.40's PDB doesn't name it. The NewPlayer event
    // the client build keeps (AFortPlayerController's sender, 7.30 0x1404040) is a different event with
    // UFortNewPlayerParams, a sibling class, so missions that wait for NewPlayerHasBegunPlay (Frostnite's Mission_Endless:
    // HandleMissionEvent_NewPlayerHasBegunPlay casts to this class, then GiveStartingInventoryToPlayers) never heard it
    // (#516). Sent to every mission, as the NewPlayer event is; a build without the class or the event name skips it.
    const FName EventName(L"NewPlayerHasBegunPlay", FNAME_Find);
    UClass* Class = StaticClass();
    if (!NewPlayerPC || !Class || EventName.IsNone() || !FindStaticFunction_SetParams() || !UFortMissionLibrary::FindStaticFunction_ExecuteMissionEvent())
    {
        return;
    }

    UObject* TransientPackage = StaticFindObject(nullptr, nullptr, L"/Engine/Transient");
    UFortNewPlayerHasBegunPlayParams* Params = NewObject<UFortNewPlayerHasBegunPlayParams>(TransientPackage, Class);
    if (Params && Params->SetParams(NewPlayerPC))
    {
        UE_LOG(LogFortMission, Log, TEXT("NewPlayerHasBegunPlay mission event for %s"), *NewPlayerPC->GetName());
        UFortMissionLibrary::ExecuteMissionEvent(NewPlayerPC, EventName, Params);
    }
#endif
}
