#include "pch.h"
#include "Online/FortGameSession.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <iterator>

namespace
{
    using FOnVerifyAuthComplete = void (*)(AFortGameSession* This, bool bWasSuccessful, const FAuthTokenMcp& AuthToken, const void* VerifyResponse, const FString& Error);

    // Each game session class's own OnVerifyAuthComplete (AFortGameSessionDedicated's override ends in a jump to
    // AFortGameSession's), by the class's vtable.
    struct FVerifyAuthOriginal
    {
        void** VTable = nullptr;
        FOnVerifyAuthComplete Original = nullptr;
    };

    FVerifyAuthOriginal VerifyAuthOriginals[8];
    int32_t VerifyAuthOriginalCount = 0;
}

void AFortGameSession::HookVTables()
{
    UObject* SessionDefault = StaticClass()->GetDefaultObject(false);
    void** SessionVTable = SessionDefault ? *reinterpret_cast<void***>(SessionDefault) : nullptr;
    if (!SessionVTable || !Finder::ResolveGameSessionVerifyAuthLive(SessionVTable) || Natives::AFortGameSession_bOnVerifyAuthCompleteTestsSuccess)
    {
        return;
    }

    // Exception to the no-hooks rule, OK'd by the owner (2026-11-02): 2.4 (CL-3858292) dropped OnVerifyAuthComplete's
    // `if (!bWasSuccessful)`, so the result of every joining player's ValidateAuth, a successful one included, kicks
    // them to the login screen ("User auth check failed."). This puts 2.1-2.3's test back: a failed verify still kicks.
    const int32_t Index = Natives::AFortGameSession_OnVerifyAuthCompleteIndex;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (!VTable || VTable[Index] == reinterpret_cast<void*>(&OnVerifyAuthCompleteHK))
        {
            continue;
        }
        if (VerifyAuthOriginalCount >= static_cast<int32_t>(std::size(VerifyAuthOriginals)))
        {
            Log::Warning(L"AFortGameSession::OnVerifyAuthComplete: more game session classes than expected");
            break;
        }
        FVerifyAuthOriginal& Entry = VerifyAuthOriginals[VerifyAuthOriginalCount];
        Entry.VTable = VTable;
        Entry.Original = reinterpret_cast<FOnVerifyAuthComplete>(VTable[Index]);
        if (Hooks::SwapVTable(L"AFortGameSession::OnVerifyAuthComplete", VTable, Index, reinterpret_cast<void*>(&OnVerifyAuthCompleteHK)))
        {
            ++VerifyAuthOriginalCount;
        }
    }
}

void AFortGameSession::OnVerifyAuthCompleteHK(AFortGameSession* This, bool bWasSuccessful, const FAuthTokenMcp& AuthToken, const void* VerifyResponse, const FString& Error)
{
    if (bWasSuccessful)
    {
        return;
    }

    void** const VTable = *reinterpret_cast<void***>(This);
    for (int32_t Index = 0; Index < VerifyAuthOriginalCount; ++Index)
    {
        if (VerifyAuthOriginals[Index].VTable == VTable)
        {
            VerifyAuthOriginals[Index].Original(This, bWasSuccessful, AuthToken, VerifyResponse, Error);
            return;
        }
    }
}
