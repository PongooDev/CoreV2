#include "pch.h"
#include "UI/FortAsyncAction_ShowPartyDialog.h"
#include "GameThread.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace
{
    // How long a card may wait to be built: the dialog builds it when it opens, a frame or two after it is made.
    constexpr ULONGLONG BuildWaitMs = 10000;

    template <typename T>
    T* PropertyOf(const UStruct* Owner, const wchar_t* Name)
    {
        return Owner ? static_cast<T*>(Owner->FindPropertyByName(FName(Name, FNAME_Find))) : nullptr;
    }

    // The dialog's left card: Description.LeftAdditionalContent.
    UObject* LeftContentOf(UObject* Action)
    {
        UStructProperty* Description = Action ? PropertyOf<UStructProperty>(Action->GetClass(), L"Description") : nullptr;
        UProperty* LeftContent = Description ? PropertyOf<UProperty>(Description->Struct, L"LeftAdditionalContent") : nullptr;
        return LeftContent ? *LeftContent->ContainerPtrToValuePtr<UObject*>(Description->ContainerPtrToValuePtr<void>(Action)) : nullptr;
    }

    // Whether a widget has its Slate widget yet: UWidget's TWeakPtr<SWidget> MyWidget and TWeakPtr<SObjectWidget>
    // MyGCWidget, the members just before NativeBindings (0xE8 and 0xF8 before 0x108 in 4.16's UWidget, as its
    // GetCachedWidget reads them), with a live shared reference.
    bool IsBuilt(UObject* Widget)
    {
        UProperty* Bindings = PropertyOf<UProperty>(Widget->GetClass(), L"NativeBindings");
        if (!Bindings)
        {
            return false;
        }
        const uintptr_t Next = reinterpret_cast<uintptr_t>(Widget) + Bindings->GetOffset_ForInternal();
        for (const uintptr_t WeakPtr : { Next - 0x20, Next - 0x10 })
        {
            const uintptr_t Controller = *reinterpret_cast<const uintptr_t*>(WeakPtr + 8);
            if (*reinterpret_cast<void* const*>(WeakPtr) && Controller && *reinterpret_cast<const int32_t*>(Controller + 8) > 0)
            {
                return true;
            }
        }
        return false;
    }

    // The card's UpdateMemberInfo again, with the MemberInfo it already has.
    bool Refresh(UObject* Card)
    {
        UFunction* Update = Card->FindFunction(FName(L"UpdateMemberInfo", FNAME_Find));
        UStructProperty* MemberInfo = PropertyOf<UStructProperty>(Card->GetClass(), L"MemberInfo");
        UStructProperty* NewMemberInfo = PropertyOf<UStructProperty>(Update, L"NewMemberInfo");
        if (!Update || !MemberInfo || !NewMemberInfo || NewMemberInfo->Struct != MemberInfo->Struct)
        {
            return false;
        }
        // The whole frame: InitializeStruct writes the Blueprint function's local variables too.
        std::vector<uint8_t> Parms(std::max<size_t>(Update->ParmsSize, static_cast<size_t>(Update->GetStructureSize())), 0);
        Update->InitializeStruct(Parms.data());
        MemberInfo->Struct->CopyScriptStruct(NewMemberInfo->ContainerPtrToValuePtr<void>(Parms.data()), MemberInfo->ContainerPtrToValuePtr<void>(Card));
        Card->ProcessEvent(Update, Parms.data());
        Update->DestroyStruct(Parms.data());
        return true;
    }

    void RefreshWhenBuilt(TWeakObjectPtr<UObject> Card, ULONGLONG Deadline)
    {
        GameThread::Enqueue([Card, Deadline]
        {
            UObject* Widget = Card.Get();
            if (!Widget)
            {
                return;
            }
            if (!IsBuilt(Widget))
            {
                if (GetTickCount64() < Deadline)
                {
                    RefreshWhenBuilt(Card, Deadline);
                }
                else
                {
                    UE_LOG(LogFort, Warning, TEXT("ShowPartyDialog: %s was never built, its power and banner stay unset"), *GetNameSafe(Widget));
                }
                return;
            }
            if (Refresh(Widget))
            {
                UE_LOG(LogFort, Log, TEXT("ShowPartyDialog: %s shows its member's power and banner"), *GetNameSafe(Widget));
            }
        });
    }

    // Manage Party Member (#94) showed every member's power as 0 and a blank banner. The lobby's player menu
    // (AthenaLobbyPlayerPanelActions) makes the dialog's left card, FullPartyMemberConnected, and calls its
    // UpdateMemberInfo at once: the rating's InterpolateToValue (UHomeBaseContext::GetTotalHomebaseRating) and the
    // banner (PlayerBanner's SetBannerOwner, the athena loadout). The card is built only when the dialog adds it
    // (ConfirmationWindow's SetupLeftAdditionalContent), and building it ran its Construct, which sets the rating to 0
    // and so stops the interpolation (UCommonNumericTextBlock::SetCurrentValue, 1.8 0x3953510), and PlayerBanner's
    // PreConstruct, which puts back the plain banner brush over the one it had drawn. Once the card is built, its
    // UpdateMemberInfo runs again.
    void RefreshCardOf(void* Result)
    {
        UObject* Action = Result ? *static_cast<UObject**>(Result) : nullptr;
        UObject* Card = LeftContentOf(Action);
        if (Card && Card->FindFunction(FName(L"UpdateMemberInfo", FNAME_Find)))
        {
            RefreshWhenBuilt(TWeakObjectPtr<UObject>(Card), GetTickCount64() + BuildWaitMs);
        }
    }
}

DEFINE_FUNCTION(UFortAsyncAction_ShowPartyDialog::execShowPartyLeaderManageDialog)
{
    ShowPartyLeaderManageDialogOG(Context, TheStack, RESULT_PARAM);
    RefreshCardOf(RESULT_PARAM);
}

DEFINE_FUNCTION(UFortAsyncAction_ShowPartyDialog::execShowPartyMemberManageDialog)
{
    ShowPartyMemberManageDialogOG(Context, TheStack, RESULT_PARAM);
    RefreshCardOf(RESULT_PARAM);
}

void UFortAsyncAction_ShowPartyDialog::HookNatives()
{
    if (UFunction* Function = FindStaticFunction_ShowPartyLeaderManageDialog(); Function && !ShowPartyLeaderManageDialogOG)
    {
        ShowPartyLeaderManageDialogOG = Function->GetNativeFunc();
        Function->SetNativeFunc(&execShowPartyLeaderManageDialog);
        Log::Info(L"Hook UFortAsyncAction_ShowPartyDialog::ShowPartyLeaderManageDialog: native");
    }
    if (UFunction* Function = FindStaticFunction_ShowPartyMemberManageDialog(); Function && !ShowPartyMemberManageDialogOG)
    {
        ShowPartyMemberManageDialogOG = Function->GetNativeFunc();
        Function->SetNativeFunc(&execShowPartyMemberManageDialog);
        Log::Info(L"Hook UFortAsyncAction_ShowPartyDialog::ShowPartyMemberManageDialog: native");
    }
}
