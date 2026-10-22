#include "pch.h"
#include "Missions/FortMission.h"
#include "AI/FortAIDirector.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Engine/World.h"
#include "Items/FortBadgeItemDefinition.h"
#include "Missions/FortMissionInfo.h"
#include "Missions/FortMissionLibrary.h"
#include "Missions/FortMissionManager.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"
#include "ReflectionParams.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Class.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UnrealType.h"

#include <algorithm>
#include <vector>

namespace
{
    class FEventParms
    {
    public:
        explicit FEventParms(UFunction* InFunction)
            : Function(InFunction)
            , Buffer(InFunction ? std::max<size_t>(InFunction->ParmsSize, InFunction->GetPropertiesSize()) + sizeof(uint64_t) : 0)
        {
            if (Function)
            {
                Function->InitializeStruct(Buffer.data());
            }
        }

        FEventParms(const FEventParms&) = delete;
        FEventParms& operator=(const FEventParms&) = delete;

        ~FEventParms()
        {
            if (Function)
            {
                Function->DestroyStruct(Buffer.data());
            }
        }

        UFunction* GetFunction() const { return Function; }
        uint8_t* GetData() { return Buffer.data(); }

        template <typename T>
        T* Find(const wchar_t* Name)
        {
            UProperty* Property = Function ? Function->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
            return Property && static_cast<size_t>(Property->GetSize()) == sizeof(T) ? reinterpret_cast<T*>(Buffer.data() + Property->GetOffset_ForInternal()) : nullptr;
        }

        bool Call(UObject* Object)
        {
            if (!Function || !Object)
            {
                return false;
            }
            Object->ProcessEvent(Function, Buffer.data());
            return true;
        }

    private:
        UFunction* Function;
        std::vector<uint8_t> Buffer;
    };

    UFunction* FindEventOverride(const UObject* Object, UFunction* NativeEvent)
    {
        return Object && NativeEvent ? Object->FindFunction(NativeEvent->GetFName()) : nullptr;
    }

    void StepEventParms(void* TheStack, UFunction* Function)
    {
        FEventParms Parms(Function);
        for (UField* Field = Function ? static_cast<UField*>(Function->Children) : nullptr; Field; Field = Field->Next)
        {
            UProperty* Property = static_cast<UProperty*>(Field);
            if ((Property->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm)
            {
                P_STACK.StepCompiledIn<UProperty>(Parms.GetData() + Property->GetOffset_ForInternal());
            }
        }
        P_FINISH;
    }
}

void AFortMission::HookCallSites()
{
    for (UFunction* Function : { StaticFunction_StopMissionAIEncounter(), StaticFunction_StopEncounterSequence() })
    {
        if (uint8_t* Call = Finder::NullGetCurrentCall(Function))
        {
            Hooks::RedirectCall(L"AFortAIDirector::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIDirector::GetCurrent));
        }
        else
        {
            Log::Error(L"Finder: AFortAIDirector::GetCurrent call in {} not found", Function ? Function->GetName().ToStdString() : std::wstring(L"(none)"));
        }
    }

    if (uint8_t* Call = Finder::SharedEmptyCallInImplementation(AFortObjectiveBase::StaticFunction_GrantRewardsByTag()))
    {
        Hooks::RedirectCall(L"AFortObjectiveBase::GrantRewardsByTag -> AFortMission::GrantRewardsByTag", Call, reinterpret_cast<void*>(&GrantRewardsByTagCall));
    }
    else
    {
        Log::Error(L"Finder: AFortMission::GrantRewardsByTag call in AFortObjectiveBase::GrantRewardsByTag not found");
    }
}

ptrdiff_t AFortMission::MissionInfoOffset()
{
    return Member_MissionInfo.GetOffsetChecked();
}

bool AFortMission::IsPrimaryMission() const
{
    UFortMissionInfo* Info = MissionInfo;
    return Info && Info->MissionType == 0;
}

TArray<AFortPlayerController*> AFortMission::GetParticipatingMissionControllers()
{
    Reflection::FParams Params(StaticFunction_GetParticipatingMissionControllers(), true);
    return Params.Call(this) ? Params.Take<TArray<AFortPlayerController*>>(L"ReturnValue") : TArray<AFortPlayerController*>();
}

ptrdiff_t AFortMission::ActiveObjectivesOffset()
{
    return Member_ActiveObjectives.GetOffsetChecked();
}

void AFortMission::ObjectiveStatusUpdated(AFortObjectiveBase* Objective, EFortObjectiveStatus NewStatus)
{
    if (Natives::AFortMission_ObjectiveStatusUpdatedIndex < 0 && !Finder::ResolveMissionObjectivesLive())
    {
        return;
    }
    CallVirtual<void>(this, Natives::AFortMission_ObjectiveStatusUpdatedIndex, Objective, NewStatus);
}

AFortMissionManager* AFortMission::GetMissionManager() const
{
    UWorld* World = GetWorld();
    for (TObjectIterator<AFortMissionManager> It; It; ++It)
    {
        if (It->GetWorld() == World && !It->IsPendingKill())
        {
            return *It;
        }
    }
    return nullptr;
}

TArray<UFortWorldItemDefinition*> AFortMission::GetRewardItemsByTag(const FGameplayTag& RewardTag)
{
    Reflection::FParams Params(StaticFunction_GetRewardItemsByTag(), true);
    Params.Set(L"RewardTag", RewardTag);
    TArray<UFortWorldItemDefinition*> Items;
    if (Params.Call(this))
    {
        Items = Params.Take<TArray<UFortWorldItemDefinition*>>(L"OutRewardItems");
    }
    return Items;
}

void AFortMission::GrantRewardsByTag(FGameplayTag RewardTag, EFortRewardType RewardType)
{
#if WITH_SERVER_CODE
    AFortMissionManager* MissionManager = Role == ROLE_Authority ? GetMissionManager() : nullptr;
    if (!MissionManager)
    {
        return;
    }

    const TArray<AFortPlayerController*> Players = GetParticipatingMissionControllers();
    for (UFortWorldItemDefinition* Item : GetRewardItemsByTag(RewardTag))
    {
        if (UFortBadgeItemDefinition* Badge = Cast<UFortBadgeItemDefinition>(reinterpret_cast<UObject*>(Item)))
        {
            MissionManager->GrantBadge(Badge, RewardType, Players);
        }
    }
#endif
}

void AFortMission::GrantRewardsByTagCall(AFortMission* This, FGameplayTag RewardTag, EFortRewardType RewardType)
{
    This->GrantRewardsByTag(RewardTag, RewardType);
}

DEFINE_FUNCTION(AFortMission::execGrantRewardsByTag)
{
    P_GET_STRUCT(FGameplayTag, Z_Param_RewardTag);
    P_GET_PROPERTY(UByteProperty, Z_Param_RewardType);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMission*>(Context)->GrantRewardsByTag(Z_Param_RewardTag, static_cast<EFortRewardType>(Z_Param_RewardType));
    P_NATIVE_END;
}

bool AFortMission::GetAdditionalMissionSuccessBadges(TArray<UFortWorldItemDefinition*>& SuccessItems)
{
    FEventParms Parms(FindEventOverride(this, StaticFunction_GetAdditionalMissionSuccessBadges()));
    TArray<UFortWorldItemDefinition*>* Items = Parms.Find<TArray<UFortWorldItemDefinition*>>(L"SuccessItems");
    bool* ReturnValue = Parms.Find<bool>(L"ReturnValue");
    if (!Items || !ReturnValue || !Parms.Call(this))
    {
        return false;
    }
    SuccessItems = std::move(*Items);
    return *ReturnValue;
}

int32_t AFortMission::GetAdditionalMissionCompletionMissionPoints(EFortMissionStatus CompletionStatus)
{
    FEventParms Parms(FindEventOverride(this, StaticFunction_GetAdditionalMissionCompletionMissionPoints()));
    EFortMissionStatus* Status = Parms.Find<EFortMissionStatus>(L"CompletionStatus");
    int32_t* ReturnValue = Parms.Find<int32_t>(L"ReturnValue");
    if (!Status || !ReturnValue)
    {
        return 0;
    }
    *Status = CompletionStatus;
    return Parms.Call(this) ? *ReturnValue : 0;
}

void AFortMission::BlueprintCustomHandleMissionEvent(const FFortMissionEvent& MissionEvent)
{
    FEventParms Parms(FindEventOverride(this, StaticFunction_BlueprintCustomHandleMissionEvent()));
    UFunction* Function = Parms.GetFunction();
    UScriptStruct* EventStruct = FFortMissionEvent::StaticStruct();
    if (!Function || !EventStruct)
    {
        return;
    }

    std::vector<uint8_t> Event(EventStruct->GetStructureSize());
    EventStruct->InitializeStruct(Event.data());
    EventStruct->CopyScriptStruct(Event.data(), &MissionEvent);
    for (UField* Field = Function->Children; Field; Field = Field->Next)
    {
        UProperty* Param = static_cast<UProperty*>(Field);
        UProperty* Member = (Param->PropertyFlags & CPF_Parm) ? EventStruct->FindPropertyByName(Param->GetFName()) : nullptr;
        if (Member && Member->GetSize() == Param->GetSize())
        {
            uint8_t* const MemberValue = Event.data() + Member->GetOffset_ForInternal();
            std::swap_ranges(MemberValue, MemberValue + Member->GetSize(), Parms.GetData() + Param->GetOffset_ForInternal());
        }
    }
    UProperty* EventParam = Function->FindPropertyByName(FName(L"MissionEvent", FNAME_Find));
    if (EventParam && EventParam->GetSize() == EventStruct->GetStructureSize())
    {
        EventStruct->CopyScriptStruct(Parms.GetData() + EventParam->GetOffset_ForInternal(), &MissionEvent);
    }

    Parms.Call(this);
    EventStruct->DestroyStruct(Event.data());
}

void AFortMission::CustomHandleMissionEvent(const FFortMissionEvent& MissionEvent)
{
    CustomHandleMissionEventOG(this, MissionEvent);

#if WITH_SERVER_CODE
    if (Role == ROLE_Authority)
    {
        BlueprintCustomHandleMissionEvent(MissionEvent);
    }
#endif
}

void AFortMission::CustomHandleMissionEventHK(AFortMission* This, const FFortMissionEvent& MissionEvent)
{
    This->CustomHandleMissionEvent(MissionEvent);
}

int32_t AFortMission::GetDaysRemainingBeforeOverPar()
{
    Reflection::FParams Params(StaticFunction_GetDaysRemainingBeforeOverPar());
    return Params.Call(this) ? Params.Take<int32_t>(L"ReturnValue") : -1;
}

bool AFortMission::IsUnderPar()
{
    return !bFiredParTimeEvent && GetDaysRemainingBeforeOverPar() > 0;
}

void AFortMission::HandleDeferredMissionEvent(const FFortMissionEvent& MissionEvent)
{
    HandleDeferredMissionEventOG(this, MissionEvent);

#if WITH_SERVER_CODE
    UFortMissionInfo* Info = MissionInfo;
    if (Role != ROLE_Authority || !Info || !Info->UnderParBadge || Info->ParTime <= 0 || bFiredParTimeEvent)
    {
        return;
    }
    if (MissionEvent.EventType != FName(L"DayPhaseChange", FNAME_Find) || static_cast<EFortMissionStatus>(MissionStatus) != EFortMissionStatus::InProgress)
    {
        return;
    }
    if (GetDaysRemainingBeforeOverPar() > 0)
    {
        return;
    }

    bFiredParTimeEvent = true;
    UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"MissionParTimeHit", FNAME_Find), MissionGuid, FGameplayTagContainer(), nullptr);
#endif
}

void AFortMission::HandleDeferredMissionEventHK(AFortMission* This, const FFortMissionEvent& MissionEvent)
{
    This->HandleDeferredMissionEvent(MissionEvent);
}

DEFINE_FUNCTION(AFortMission::execGetAdditionalMissionSuccessBadges)
{
    StepEventParms(TheStack, StaticFunction_GetAdditionalMissionSuccessBadges());
    P_NATIVE_BEGIN;
    *static_cast<bool*>(RESULT_PARAM) = false;
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortMission::execGetAdditionalMissionCompletionMissionPoints)
{
    StepEventParms(TheStack, StaticFunction_GetAdditionalMissionCompletionMissionPoints());
    P_NATIVE_BEGIN;
    *static_cast<int32_t*>(RESULT_PARAM) = 0;
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortMission::execBlueprintCustomHandleMissionEvent)
{
    StepEventParms(TheStack, StaticFunction_BlueprintCustomHandleMissionEvent());
}

void AFortMission::HookNatives()
{
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

    if (UFunction* Function = StaticFunction_GrantRewardsByTag())
    {
        Function->SetNativeFunc(&execGrantRewardsByTag);
        Log::Info(L"Hook AFortMission::GrantRewardsByTag: native");
    }
    if (UFunction* Function = StaticFunction_GetAdditionalMissionSuccessBadges())
    {
        Function->SetNativeFunc(&execGetAdditionalMissionSuccessBadges);
        Log::Info(L"Hook AFortMission::GetAdditionalMissionSuccessBadges: native");
    }
    if (UFunction* Function = StaticFunction_GetAdditionalMissionCompletionMissionPoints())
    {
        Function->SetNativeFunc(&execGetAdditionalMissionCompletionMissionPoints);
        Log::Info(L"Hook AFortMission::GetAdditionalMissionCompletionMissionPoints: native");
    }
    if (UFunction* Function = StaticFunction_BlueprintCustomHandleMissionEvent())
    {
        Function->SetNativeFunc(&execBlueprintCustomHandleMissionEvent);
        Log::Info(L"Hook AFortMission::BlueprintCustomHandleMissionEvent: native");
    }
}

void AFortMission::HookVTables()
{
    if (Natives::AFortMission_ObjectiveStatusUpdatedIndex < 0)
    {
        Finder::ResolveMissionObjectivesLive();
    }

    if (Natives::AFortMission_CustomHandleMissionEventIndex < 0 && !Finder::ResolveMissionEventLive())
    {
        return;
    }

    const int32_t Index = Natives::AFortMission_CustomHandleMissionEventIndex;
    UObject* MissionDefault = StaticClass()->GetDefaultObject(false);
    void* const Original = MissionDefault ? (*reinterpret_cast<void***>(MissionDefault))[Index] : nullptr;
    CustomHandleMissionEventOG = reinterpret_cast<decltype(CustomHandleMissionEventOG)>(Original);
    const bool bHasDeferred = Finder::ResolveMissionParTimeLive(reinterpret_cast<uintptr_t>(Original));
    const int32_t DeferredIndex = Natives::AFortMission_HandleDeferredMissionEventIndex;
    void* const DeferredOriginal = bHasDeferred && MissionDefault ? (*reinterpret_cast<void***>(MissionDefault))[DeferredIndex] : nullptr;
    HandleDeferredMissionEventOG = reinterpret_cast<decltype(HandleDeferredMissionEventOG)>(DeferredOriginal);
    for (TObjectIterator<UClass> It; Original && It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Original)
        {
            Hooks::SwapVTable(L"AFortMission::CustomHandleMissionEvent", VTable, Index, reinterpret_cast<void*>(&CustomHandleMissionEventHK));
        }
        if (VTable && DeferredOriginal && VTable[DeferredIndex] == DeferredOriginal)
        {
            Hooks::SwapVTable(L"AFortMission::HandleDeferredMissionEvent", VTable, DeferredIndex, reinterpret_cast<void*>(&HandleDeferredMissionEventHK));
        }
    }
}
