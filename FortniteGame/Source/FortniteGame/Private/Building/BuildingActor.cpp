#include "pch.h"
#include "Building/BuildingActor.h"
#include "Finder.h"
#include "Hooks.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Missions/FortMissionInterface.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <vector>

void ABuildingActor::PostInitializeComponents()
{
    PostInitializeComponentsOG(this);

#if WITH_SERVER_CODE
    // 2.2 to 2.4.2's ABuildingActor::PostInitializeComponents (2.4.2 0x472580) gives a building that implements
    // UFortMissionInterface its mission event params, as AFortMissionState's does for missions and objectives: each
    // null object property of a UFortMissionEventParams class gets a new object of it. 2.5 (0x9F69F0) lost that call.
    // Wounded Survivor's MED-bot (B_MEDBot) implements the interface in its blueprint and sends its BluGlo deposit as
    // GenDeposit->SetParams(Player) then ExecuteMissionEventEx(TryDepositItem, Params: GenDeposit). With GenDeposit
    // null the event carried no params and no instigator, so Gen_Deposit's TryDepositItem had no player to take the
    // BluGlo from and interacting did nothing (CoreV2 #219).
    if (UKismetSystemLibrary::DoesImplementInterface(this, UFortMissionInterface::StaticClass()))
    {
        Natives::InitializeMissionEventParams(this);
    }
#endif
}

void ABuildingActor::HookVTables()
{
    if (!Finder::ResolveMissionEventParamsLive())
    {
        return;
    }

    const int32_t Index = Natives::AActor_PostInitializeComponentsIndex;
    UObject* BuildingDefault = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** BuildingVTable = BuildingDefault ? *reinterpret_cast<void***>(BuildingDefault) : nullptr;
    const uintptr_t PostInitialize = BuildingVTable ? reinterpret_cast<uintptr_t>(BuildingVTable[Index]) : 0;
    const uintptr_t PostInitializeEnd = PostInitialize ? Finder::FunctionEnd(PostInitialize) : 0;
    if (!PostInitializeEnd)
    {
        Log::Error(L"Finder: ABuildingActor::PostInitializeComponents not found");
        return;
    }
    const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = Finder::DirectCalls(PostInitialize, PostInitializeEnd);
    const uintptr_t Initialize = reinterpret_cast<uintptr_t>(Natives::InitializeMissionEventParams);
    if (std::any_of(Calls.begin(), Calls.end(), [Initialize](const auto& Call) { return Call.second == Initialize; }))
    {
        return;
    }

    // Classes that inherit it get the hook in their vtable; the ones that override it (ABuildingGameplayActor's, 2.5
    // 0xA2B950) call it as Super, or jump to it, and those calls get it instead.
    PostInitializeComponentsOG = reinterpret_cast<decltype(PostInitializeComponentsOG)>(PostInitialize);
    std::vector<uintptr_t> Overrides;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        const uintptr_t Entry = VTable ? reinterpret_cast<uintptr_t>(VTable[Index]) : 0;
        if (Entry == PostInitialize)
        {
            Hooks::SwapVTable(L"ABuildingActor::PostInitializeComponents", VTable, Index, reinterpret_cast<void*>(&PostInitializeComponentsHK));
        }
        else if (Entry && Entry != reinterpret_cast<uintptr_t>(&PostInitializeComponentsHK) && std::find(Overrides.begin(), Overrides.end(), Entry) == Overrides.end())
        {
            Overrides.push_back(Entry);
        }
    }
    for (const uintptr_t Site : Finder::CallsTo(PostInitialize, true))
    {
        if (std::find(Overrides.begin(), Overrides.end(), Finder::FunctionStart(Site)) != Overrides.end())
        {
            Hooks::RedirectCall(L"ABuildingActor::PostInitializeComponents (Super)", reinterpret_cast<uint8_t*>(Site), reinterpret_cast<void*>(&PostInitializeComponentsHK));
        }
    }
}

void ABuildingActor::PostInitializeComponentsHK(ABuildingActor* This)
{
    This->PostInitializeComponents();
}

float ABuildingActor::GetHealth() const
{
    Reflection::FParams Params(StaticFunction_GetHealth());
    return Params.Call(const_cast<ABuildingActor*>(this)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}

FVector ABuildingActor::GetCentroid() const
{
    Reflection::FParams Params(StaticFunction_GetCentroid());
    return Params.Call(const_cast<ABuildingActor*>(this)) ? Params.Take<FVector>(L"ReturnValue") : FVector();
}

bool ABuildingActor::CountsTowardStructureLimit() const
{
    if (!Member_bFireBuiltAndDestroyedEvents.IsAvailable() || !Member_bPlayerPlaced.IsAvailable() || !Member_bDestroyed.IsAvailable())
    {
        return false;
    }
    const bool bPlayerBuilt = OwnerPersistentID != -1 || bPlayerPlaced;
    return !bDestroyed && bFireBuiltAndDestroyedEvents && bPlayerBuilt;
}

float ABuildingActor::GetMaxHealth() const
{
    Reflection::FParams Params(StaticFunction_GetMaxHealth());
    return Params.Call(const_cast<ABuildingActor*>(this)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
}
