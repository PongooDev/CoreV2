#pragma once

#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/Stack.h"

// The party member dialogs (Manage Party Member) the lobby's player menu opens (FortniteUI).
class UFortAsyncAction_ShowPartyDialog : public UObject
{
    DefineUnrealClass(UFortAsyncAction_ShowPartyDialog, FortniteUI)
    DefineUFunction(ShowPartyLeaderManageDialog)
    DefineUFunction(ShowPartyMemberManageDialog)

public:
    static void HookNatives();

private:
    DECLARE_FUNCTION(execShowPartyLeaderManageDialog);
    DECLARE_FUNCTION(execShowPartyMemberManageDialog);

    static inline UFunction::FNativeFuncPtr ShowPartyLeaderManageDialogOG = nullptr;
    static inline UFunction::FNativeFuncPtr ShowPartyMemberManageDialogOG = nullptr;
};
