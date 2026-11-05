#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Log.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

// Squad Comms (3.5's patch notes: "Enemy Spotted", "Need Meds", "Need Shields", "Need Materials", "Need Weapon", "Need
// Ammo", on V). The wheel's client (3.5.1 0xA34270) shows the callout over its own pawn (CurrQuickChatBrush), starts its
// cooldown (MinQuickChatCooldown, QuickChatOffCooldownTime) and sends ServerPlaySquadQuickChatMessage. The squad sees a
// callout through the sender's player state: ReplicatedTeamMemberState, whose OnRep (0xA66160) tells the HUD and ends it
// on a timer. Nothing in 3.5.1 sets it: the RPC's _Implementation (slot 0x365, 0xA2D450) only destroys SenderID, and
// AFortPlayerStateAthena::SetTeamMemberState (21.40's name) has no body left (the OnRep's timer at 0xA70710 is the only
// other writer, setting None). ClientReceiveSquadQuickChatMessage_Implementation (slot 0x363) is the shared empty function
// on the client, and the client stub that sends it is gone with the server code.
//
// 3.3 has the RPC but no team member state; it is left alone.
void AFortPlayerControllerAthena::ServerPlaySquadQuickChatMessage_Implementation(const FAthenaQuickChatActiveEntry& ChatEntry)
{
#if WITH_SERVER_CODE
    AFortPlayerStateAthena* SenderState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    UAthenaQuickChatBank* Bank = ChatEntry.Bank;
    const int32_t Index = ChatEntry.Index;
    if (!SenderState || !Bank || Index < 0 || Index >= Bank->ChatOptions.Num())
    {
        Log::Warning(L"Squad Comms: {} sent a callout that isn't on the wheel (bank {}, index {})", GetName().ToStdString(), Bank ? Bank->GetName().ToStdString() : L"none", Index);
        return;
    }

    // The callout's TeamCommType is what the squad's HUD shows over the sender.
    const uint8_t TeamCommType = Bank->ChatOptions[Index].TeamCommType;
    SenderState->SetTeamMemberState(TeamCommType);

    // The sender's squad (their team where the build has no squads) gets the callout too.
    UWorld* World = GetWorld();
    AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
    int32_t Receivers = 0;
    if (GameState)
    {
        const bool bSquads = AFortPlayerStateAthena::HasSquadId() && static_cast<uint8_t>(SenderState->SquadId) != 0;
        for (APlayerState* Other : GameState->PlayerArray)
        {
            AFortPlayerStateAthena* OtherState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(Other));
            if (!OtherState || OtherState == SenderState || OtherState->TeamIndex != SenderState->TeamIndex || (bSquads && OtherState->SquadId != SenderState->SquadId))
            {
                continue;
            }
            if (AFortPlayerControllerAthena* OtherController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(OtherState->GetOwner())))
            {
                OtherController->ClientReceiveSquadQuickChatMessage(ChatEntry, this);
                ++Receivers;
            }
        }
    }
    Log::Info(L"Squad Comms: {} calls out {} ({} index {}), {} squadmates told", GetName().ToStdString(), ETeamMemberState::StaticEnumInfo().GetValueName(TeamCommType), Bank->GetName().ToStdString(), Index, Receivers);
#endif
}

void AFortPlayerControllerAthena::ClientReceiveSquadQuickChatMessage(const FAthenaQuickChatActiveEntry& ChatEntry, AFortPlayerController* SenderPC)
{
    UFunction* Function = FindStaticFunction_ClientReceiveSquadQuickChatMessage();
    Reflection::FParams Params(Function, true);
    if (Function && Params.CopyStruct(L"ChatEntry", &ChatEntry) && Params.Set(L"SenderPC", SenderPC))
    {
        Params.Call(this);
    }
}

void AFortPlayerControllerAthena::HookQuickChatVTables()
{
#if WITH_SERVER_CODE
    UFunction* ServerPlay = FindStaticFunction_ServerPlaySquadQuickChatMessage();
    if (!ServerPlay || !AFortPlayerStateAthena::HasTeamMemberState() || !FAthenaQuickChatActiveEntry::StaticStruct())
    {
        return;
    }
    const int32_t Index = Finder::ImplementationIndex(ServerPlay);
    UObject* AthenaDefault = StaticClass()->GetDefaultObject(false);
    void** AthenaVTable = AthenaDefault ? *reinterpret_cast<void***>(AthenaDefault) : nullptr;
    if (Index < 0 || !AthenaVTable)
    {
        Log::Error(L"Finder: AFortPlayerControllerAthena::ServerPlaySquadQuickChatMessage_Implementation slot not found");
        return;
    }

    // Only a stripped body: 3.5.1's is 0x62 bytes, SenderID's destructor and nothing else. A body that sets the team
    // member state or tells the squad is far longer.
    void* const Implementation = AthenaVTable[Index];
    const uintptr_t Begin = reinterpret_cast<uintptr_t>(Implementation);
    const uintptr_t End = Finder::FunctionEnd(Begin);
    if (!End || End - Begin > 0x80)
    {
        Log::Info(L"Finder: AFortPlayerControllerAthena::ServerPlaySquadQuickChatMessage_Implementation (slot 0x{:X}) is native in this build", Index);
        return;
    }

    ServerPlaySquadQuickChatMessage_ImplementationOG = reinterpret_cast<decltype(ServerPlaySquadQuickChatMessage_ImplementationOG)>(Implementation);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Implementation)
        {
            Hooks::SwapVTable(L"AFortPlayerControllerAthena::ServerPlaySquadQuickChatMessage_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerPlaySquadQuickChatMessage_ImplementationHK));
        }
    }
#endif
}

void AFortPlayerControllerAthena::ServerPlaySquadQuickChatMessage_ImplementationHK(AFortPlayerControllerAthena* This, FAthenaQuickChatActiveEntry* ChatEntry, void* SenderID)
{
    if (ChatEntry)
    {
        This->ServerPlaySquadQuickChatMessage_Implementation(*ChatEntry);
    }
    // The stripped body: SenderID's destructor, as the callee owns its by-value parameters.
    ServerPlaySquadQuickChatMessage_ImplementationOG(This, ChatEntry, SenderID);
}
