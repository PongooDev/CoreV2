#include "pch.h"
#include "UI/FortPartyContext.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Containers/ScriptArray.h"
#include "Engine/World.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameThread.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "ReflectionParams.h"
#include "UI/FortUITeamInfo.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace PartyBarTypes
{
    class UBlueprintContextLibrary : public UObject
    {
        DefineUnrealClass(UBlueprintContextLibrary, BlueprintContext)
        DefineUFunction(GetContext)
    };

    class UUserWidget : public UObject
    {
        DefineUnrealClass(UUserWidget, UMG)
        DefineUFunction(GetOwningPlayer)
    };

    class UWidget : public UObject
    {
        DefineUnrealClass(UWidget, UMG)
        DefineUFunction(SetVisibility)
    };

    class UTextBlock : public UObject
    {
        DefineUnrealClass(UTextBlock, UMG)
        DefineUFunction(SetText)
    };
}

namespace
{
    // ESlateVisibility.
    constexpr uint8_t Collapsed = 1;
    constexpr uint8_t SelfHitTestInvisible = 4;

    // How often the banners follow the party: nothing in the front end tells them when the party changes.
    constexpr ULONGLONG RefreshMs = 1000;
    // How often the object array is searched for banners while some are known (the main menu makes its bar once).
    constexpr ULONGLONG RescanMs = 5000;

    constexpr const wchar_t* AthenaPartyMemberClassPath = L"/Game/Athena/UI/Frontend/Parties/FullPartyMemberAthena.FullPartyMemberAthena_C";

    struct FTrackedMember
    {
        TWeakObjectPtr<UObject> Widget;
        std::wstring Shown;
        bool bLogged = false;
    };

    std::vector<FTrackedMember> Tracked;
    TWeakObjectPtr<UObject> AthenaPartyMemberClass;
    ULONGLONG NextRefresh = 0;
    ULONGLONG NextRescan = 0;

    template <typename T>
    T* PropertyOf(const UStruct* Owner, const wchar_t* Name)
    {
        return Owner ? static_cast<T*>(Owner->FindPropertyByName(FName(Name, FNAME_Find))) : nullptr;
    }

    template <typename T>
    T* ValueOf(UObject* Object, const wchar_t* Name)
    {
        UProperty* Property = Object ? PropertyOf<UProperty>(Object->GetClass(), Name) : nullptr;
        return Property ? Property->ContainerPtrToValuePtr<T>(Object) : nullptr;
    }

    UObject* WidgetOf(UObject* Object, const wchar_t* Name)
    {
        UObject** Value = ValueOf<UObject*>(Object, Name);
        return Value ? *Value : nullptr;
    }

    // A function's parameters, initialized and destroyed with it. The buffer holds the whole frame: InitializeStruct
    // writes the function's local variables too.
    class FCall
    {
    public:
        explicit FCall(UFunction* InFunction)
            : Function(InFunction)
        {
            if (Function)
            {
                Parms.assign(std::max<size_t>(Function->ParmsSize, static_cast<size_t>(Function->GetStructureSize())), 0);
                Function->InitializeStruct(Parms.data());
            }
        }

        ~FCall()
        {
            if (Function)
            {
                Function->DestroyStruct(Parms.data());
            }
        }

        FCall(const FCall&) = delete;
        FCall& operator=(const FCall&) = delete;

        template <typename T = UProperty>
        T* Property(const wchar_t* Name) const
        {
            return PropertyOf<T>(Function, Name);
        }

        void* Param(const wchar_t* Name)
        {
            UProperty* Found = Property(Name);
            return Found ? Found->ContainerPtrToValuePtr<void>(Parms.data()) : nullptr;
        }

        bool Call(UObject* Object)
        {
            if (!Function || !Object)
            {
                return false;
            }
            Object->ProcessEvent(Function, Parms.data());
            return true;
        }

    private:
        UFunction* Function;
        std::vector<uint8_t> Parms;
    };

    UObject* OwningPlayerOf(UObject* Widget)
    {
        Reflection::FParams Params(PartyBarTypes::UUserWidget::StaticFunction_GetOwningPlayer());
        Params.Call(Widget);
        UObject** PlayerController = Params.Get<UObject*>(L"ReturnValue");
        return PlayerController ? *PlayerController : nullptr;
    }

    // The front end's controller is an AFortPlayerControllerFrontEnd; a match's is an AFortPlayerControllerAthena.
    bool IsInFrontEnd(UObject* Widget)
    {
        UObject* PlayerController = OwningPlayerOf(Widget);
        return PlayerController && !PlayerController->IsA(AFortPlayerControllerAthena::StaticClass());
    }

    // The party as the Save the World bar's UFortPlayerTrackerBase::SetTeamMember reads it: slot 0 is the local player's
    // own UFortPartyContext::GetLocalPlayerTeamMemberInfo, slot N the local team's GetTeamMembers()[N].
    class FPartySlots
    {
    public:
        explicit FPartySlots(UObject* Widget)
            : LocalCall(UFortPartyContext::StaticFunction_GetLocalPlayerTeamMemberInfo())
            , TeamCall(UFortUITeamInfo::StaticFunction_GetTeamMembers())
        {
            UFortPartyContext* Party = UFortPartyContext::Get(Widget);
            UStructProperty* LocalInfo = LocalCall.Property<UStructProperty>(L"LocalPlayerInfo");
            if (!Party || !LocalInfo)
            {
                return;
            }
            Struct = LocalInfo->Struct;
            if (LocalCall.Call(Party))
            {
                Local = static_cast<const uint8_t*>(LocalCall.Param(L"LocalPlayerInfo"));
            }

            UFortUITeamInfo* Team = Party->GetLocalPlayerTeam();
            UArrayProperty* TeamMembers = TeamCall.Property<UArrayProperty>(L"TeamMembers");
            UProperty* Inner = TeamMembers ? static_cast<UProperty*>(TeamMembers->Inner) : nullptr;
            if (Team && Inner && TeamCall.Call(Team))
            {
                const FScriptArray* Array = static_cast<const FScriptArray*>(TeamCall.Param(L"TeamMembers"));
                Members = static_cast<const uint8_t*>(Array->GetData());
                Num = Array->Num();
                Stride = Inner->ElementSize;
            }

            MemberUniqueId = PropertyOf<UProperty>(Struct, L"MemberUniqueId");
            PlayerName = PropertyOf<UProperty>(Struct, L"PlayerName");
            bPartyLeader = PropertyOf<UBoolProperty>(Struct, L"bPartyLeader");
            NumPlayersInParty = PropertyOf<UProperty>(Struct, L"NumPlayersInParty");
        }

        // The member in a slot, or null for an open slot.
        const uint8_t* Slot(int32_t Index) const
        {
            const uint8_t* Info = Index == 0 ? Local : (Index > 0 && Index < Num ? Members + static_cast<size_t>(Index) * Stride : nullptr);
            return Info && MemberUniqueId && MemberIdOf(Info).IsValid() ? Info : nullptr;
        }

        const FUniqueNetIdRepl& MemberIdOf(const uint8_t* Info) const
        {
            return *MemberUniqueId->ContainerPtrToValuePtr<FUniqueNetIdRepl>(Info);
        }

        const void* PlayerNameOf(const uint8_t* Info) const
        {
            return PlayerName ? PlayerName->ContainerPtrToValuePtr<void>(Info) : nullptr;
        }

        // The crown the Save the World bar shows: the leader of a party of more than one.
        bool ShowsLeader(const uint8_t* Info) const
        {
            const int32_t Players = NumPlayersInParty ? *NumPlayersInParty->ContainerPtrToValuePtr<int32_t>(Info) : 0;
            return bPartyLeader && bPartyLeader->GetPropertyValue_InContainer(Info) && Players > 1;
        }

        UScriptStruct* Struct = nullptr;

    private:
        FCall LocalCall;
        FCall TeamCall;
        const uint8_t* Local = nullptr;
        const uint8_t* Members = nullptr;
        int32_t Num = 0;
        int32_t Stride = 0;
        UProperty* MemberUniqueId = nullptr;
        UProperty* PlayerName = nullptr;
        UBoolProperty* bPartyLeader = nullptr;
        UProperty* NumPlayersInParty = nullptr;
    };

    void CallEvent(UObject* Object, const wchar_t* Name)
    {
        FCall Event(Object ? Object->FindFunction(FName(Name, FNAME_Find)) : nullptr);
        Event.Call(Object);
    }

    void SetVisibility(UObject* Widget, uint8_t Visibility)
    {
        if (!Widget || !Widget->IsA(PartyBarTypes::UWidget::StaticClass()))
        {
            return;
        }
        Reflection::FParams Params(PartyBarTypes::UWidget::StaticFunction_SetVisibility());
        if (Params.Set(L"InVisibility", Visibility))
        {
            Params.Call(Widget);
        }
    }

    void SetText(UObject* TextBlock, const void* Text)
    {
        UFunction* Function = PartyBarTypes::UTextBlock::StaticFunction_SetText();
        UProperty* InText = PropertyOf<UProperty>(Function, L"InText");
        if (!TextBlock || !Text || !InText || !TextBlock->IsA(PartyBarTypes::UTextBlock::StaticClass()))
        {
            return;
        }
        // The parameter borrows the member's FText: ProcessEvent copies parameters in and out bitwise and destroys
        // none of them, SetText keeps its own copy, and the member outlives the call.
        std::vector<uint8_t> Parms(Function->ParmsSize, 0);
        std::memcpy(InText->ContainerPtrToValuePtr<void>(Parms.data()), Text, static_cast<size_t>(InText->ElementSize));
        TextBlock->ProcessEvent(Function, Parms.data());
    }

    bool UpdateMemberInfo(UObject* Member, UScriptStruct* Struct, const uint8_t* Info)
    {
        FCall Update(Member->FindFunction(FName(L"UpdateMemberInfo", FNAME_Find)));
        UStructProperty* NewMemberInfo = Update.Property<UStructProperty>(L"NewMemberInfo");
        if (!NewMemberInfo || NewMemberInfo->Struct != Struct)
        {
            return false;
        }
        Struct->CopyScriptStruct(Update.Param(L"NewMemberInfo"), Info);
        return Update.Call(Member);
    }

    // One banner of the bar. UpdateMemberInfo picks open, connecting or connected from the party context's
    // GetPartyMemberDisplayState, as the Save the World bar does; its ShowConnected gives up on the name (it reads the
    // match's TeamMembers), so the name is set here. The kill count is the match's and stays hidden.
    std::wstring Show(UObject* Member, const FPartySlots& Party)
    {
        const int32_t* Index = ValueOf<int32_t>(Member, L"TeamMemberIndex");
        const uint8_t* Info = Index ? Party.Slot(*Index) : nullptr;
        std::wstring Shown;

        if (Info && UpdateMemberInfo(Member, Party.Struct, Info))
        {
            SetText(WidgetOf(Member, L"PlayerName"), Party.PlayerNameOf(Info));
            SetVisibility(WidgetOf(Member, L"LeaderImageOverlay"), Party.ShowsLeader(Info) ? SelfHitTestInvisible : Collapsed);
            Shown = Party.MemberIdOf(Info).ToString().ToStdString();
        }
        else
        {
            CallEvent(Member, L"ShowOpen");
            SetVisibility(WidgetOf(Member, L"LeaderImageOverlay"), Collapsed);
        }
        SetVisibility(WidgetOf(Member, L"KillsText"), Collapsed);
        SetVisibility(WidgetOf(Member, L"Image_1"), Collapsed);
        return Shown;
    }

    // Only the front end's world: a match's banners keep the game's own path, and the search stays out of matches.
    bool IsFrontEndWorld()
    {
        UWorld* World = Natives::GWorld ? static_cast<UWorld*>(GWorld) : nullptr;
        return World && World->GetName().ToStdString() == L"Frontend";
    }

    // The banners: every live FullPartyMemberAthena widget. The class exists once the main menu has loaded it.
    void FindMembers()
    {
        UObject* Class = AthenaPartyMemberClass.Get();
        if (!Class)
        {
            Class = StaticFindObject(nullptr, nullptr, AthenaPartyMemberClassPath, false);
            AthenaPartyMemberClass = Class;
        }
        const FUObjectArray* Array = Natives::GUObjectArray;
        if (!Class || !Array)
        {
            return;
        }

        for (int32_t Index = 0; Index < Array->GetObjectArrayNum(); ++Index)
        {
            const FUObjectItem* Item = Array->IndexToObject(Index);
            UObject* Object = Item ? reinterpret_cast<UObject*>(Item->Object) : nullptr;
            if (!Object || Item->IsUnreachable() || Item->IsPendingKill() || reinterpret_cast<UObject*>(Object->GetClass()) != Class || Object->IsTemplate())
            {
                continue;
            }
            const bool bKnown = std::any_of(Tracked.begin(), Tracked.end(), [Object](const FTrackedMember& Known) { return Known.Widget.Get() == Object; });
            if (!bKnown)
            {
                Tracked.push_back(FTrackedMember{ TWeakObjectPtr<UObject>(Object) });
            }
        }
    }

    void Refresh()
    {
        std::erase_if(Tracked, [](const FTrackedMember& Member) { return !Member.Widget.Get(); });
        if (!IsFrontEndWorld())
        {
            return;
        }
        const ULONGLONG Now = GetTickCount64();
        if (Tracked.empty() || Now >= NextRescan)
        {
            NextRescan = Now + RescanMs;
            FindMembers();
        }

        std::unique_ptr<FPartySlots> Party;
        for (FTrackedMember& Member : Tracked)
        {
            UObject* Widget = Member.Widget.Get();
            if (!IsInFrontEnd(Widget))
            {
                continue;
            }
            if (!Party)
            {
                Party = std::make_unique<FPartySlots>(Widget);
            }

            std::wstring Shown = Show(Widget, *Party);
            if (!Member.bLogged || Shown != Member.Shown)
            {
                const int32_t* Index = ValueOf<int32_t>(Widget, L"TeamMemberIndex");
                UE_LOG(LogFort, Log, TEXT("FullPartyBarAthena: slot %d %s%s"), Index ? *Index : -1, Shown.empty() ? L"is open" : L"shows ", Shown.c_str());
                Member.Shown = std::move(Shown);
                Member.bLogged = true;
            }
        }
    }

    // Every frame on the game thread, a refresh once a second.
    void Tick()
    {
        const ULONGLONG Now = GetTickCount64();
        if (Now >= NextRefresh)
        {
            NextRefresh = Now + RefreshMs;
            Refresh();
        }
        GameThread::Enqueue(&Tick);
    }
}

UFortPartyContext* UFortPartyContext::Get(UObject* ContextObject)
{
    UClass* Library = PartyBarTypes::UBlueprintContextLibrary::StaticClass();
    UClass* Class = StaticClass();
    if (!ContextObject || !Library || !Class)
    {
        return nullptr;
    }
    Reflection::FParams Params(PartyBarTypes::UBlueprintContextLibrary::StaticFunction_GetContext());
    if (!Params.Set(L"ContextObject", ContextObject) || !Params.Set(L"Class", Class))
    {
        return nullptr;
    }
    Params.Call(Library->GetDefaultObject());
    UObject** Context = Params.Get<UObject*>(L"ReturnValue");
    return Context && *Context && (*Context)->IsA(Class) ? reinterpret_cast<UFortPartyContext*>(*Context) : nullptr;
}

UFortUITeamInfo* UFortPartyContext::GetLocalPlayerTeam()
{
    Reflection::FParams Params(StaticFunction_GetLocalPlayerTeam());
    Params.Call(this);
    UObject** Team = Params.Get<UObject*>(L"ReturnValue");
    return Team ? reinterpret_cast<UFortUITeamInfo*>(*Team) : nullptr;
}

// 1.8.1 gave the Battle Royale main menu (MainMenu's ConfigureSubGameWidgets) a party bar of its own,
// FullPartyBarAthena; 1.8's showed none. Its four banners (FullPartyMemberAthena) are the match's squad: each reads the
// owning AFortPlayerControllerAthena's TeamMembers and follows AthenaHUDContext's team events. The lobby's controller is
// an AFortPlayerControllerFrontEnd, so every banner's cast fails and it shows open ("+"), the local player's too
// (Core-Backend #104). In the front end each banner shows the party instead, slot by slot as the Save the World bar
// (FullPartyBar) does. No engine function is hooked: once a second, in the front end's world, Core finds the banners in
// the object array and fills them.
void UFortPartyContext::StartAthenaPartyBar()
{
    static bool bStarted = false;
    if (!bStarted)
    {
        bStarted = GameThread::Enqueue(&Tick);
    }
}
