#include "pch.h"
#include "Player/FortPlayerControllerOutpost.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void AFortPlayerControllerOutpost::ServerLeaveOutpostAsParty_Implementation()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameModeZone* GameMode = World ? Cast<AFortGameModeZone>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    if (!GameMode || !Finder::ResolveLeaveOutpostLive())
    {
        return;
    }

    Natives::AFortGameMode_SendStayWithParty(GameMode);
    CallVirtual<void>(GameMode, Natives::AGameMode_ReturnToMainMenuHostIndex);
#endif
}

void AFortPlayerControllerOutpost::HookVTables()
{
    const int32_t Index = Finder::ImplementationIndex(StaticFunction_ServerLeaveOutpostAsParty());
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerControllerOutpost::ServerLeaveOutpostAsParty_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPlayerControllerOutpost::ServerLeaveOutpostAsParty_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerLeaveOutpostAsParty_ImplementationHK));
        }
    }
}

void AFortPlayerControllerOutpost::ServerLeaveOutpostAsParty_ImplementationHK(AFortPlayerControllerOutpost* This)
{
    This->ServerLeaveOutpostAsParty_Implementation();
}
