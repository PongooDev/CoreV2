#include "pch.h"
#include "FortGameModeOutpost.h"
#include "BackendRequest.h"
#include "Engine/World.h"
#include "FortGameState.h"
#include "FortZoneTheme.h"
#include "GameThread.h"
#include "Logging/LogMacros.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Templates/Casts.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <format>
#include <memory>

DEFINE_LOG_CATEGORY_STATIC(LogFortOutpost, Log, All);

namespace
{
    // How often a running Storm Shield asks again, so a change in the admin panel reaches it, and how long it waits for
    // its zone theme before saying it has none.
    constexpr auto AskInterval = std::chrono::seconds(30);
    constexpr auto ThemeWait = std::chrono::seconds(60);

    struct FStructureLimitWatch
    {
        std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point NextCheck{};
        std::chrono::steady_clock::time_point NextAsk{};
        // The Storm Shield's outpost item, Outpost:outpostcore_pve_0N, once its zone theme is known.
        std::wstring TemplateId;
        bool bAsking = false;
        // The backend has no such route (one from before Core-Backend #115): it isn't asked again.
        bool bUnsupported = false;
        bool bWarnedNoTheme = false;
        // The backend's limit, 0 while it has none (the game's own then).
        int32_t Limit = 0;
        // The game's own, from before Core changed it, to put back when the backend has none any more; -1 while
        // Core hasn't changed it.
        int32_t GameLimit = -1;
    };

    AFortGameStateZone* GameStateOf(AFortGameModeOutpost* Outpost)
    {
        UWorld* World = Outpost ? Outpost->GetWorld() : nullptr;
        return World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    }

    std::wstring OutpostTemplateId(AFortGameStateZone* GameState)
    {
        UFortZoneTheme* ZoneTheme = static_cast<UFortZoneTheme*>(GameState->ZoneTheme);
        std::wstring Name = ZoneTheme ? ZoneTheme->GetCloudSaveItemName().ToStdString() : std::wstring();
        std::transform(Name.begin(), Name.end(), Name.begin(), [](wchar_t Character) { return static_cast<wchar_t>(std::towlower(Character)); });
        return Name.empty() ? Name : L"Outpost:" + Name;
    }

    void Ask(const std::shared_ptr<FStructureLimitWatch>& Watch)
    {
        Watch->bAsking = true;
        BackendRequest::GetJson(std::format(L"structure limit of {}", Watch->TemplateId), L"/fortnite/api/game/v2/world/structurelimit?templateId=" + Watch->TemplateId,
            FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [Watch](uint32_t Status, const std::string& Body)
        {
            // {templateId, stormShield, maxStructures}: a number, or null for the game's own. Anything else (a backend
            // from before it, no answer) keeps what the Storm Shield has.
            const std::string Field = Status == 200 ? BackendRequest::JsonField(Body, "maxStructures") : std::string();
            const bool bAnswered = Field == "null" || (!Field.empty() && std::isdigit(static_cast<unsigned char>(Field[0])));
            const int32_t Limit = bAnswered && Field != "null" ? std::clamp(std::atoi(Field.c_str()), 0, 100000) : 0;
            GameThread::Enqueue([Watch, Status, bAnswered, Limit]()
            {
                Watch->bAsking = false;
                if (Status == 404 && !Watch->bUnsupported)
                {
                    Watch->bUnsupported = true;
                    UE_LOG(LogFortOutpost, Warning, TEXT("KeepStructureLimit: the backend has no structure limits (Core-Backend #115), so %s keeps the game's"), Watch->TemplateId.c_str());
                }
                if (bAnswered)
                {
                    Watch->Limit = Limit;
                }
            });
        });
    }

    void Check(AFortGameModeOutpost* Outpost, const std::shared_ptr<FStructureLimitWatch>& Watch, std::chrono::steady_clock::time_point Now)
    {
        AFortGameStateZone* GameState = GameStateOf(Outpost);
        int32_t Total = 0;
        int32_t Current = 0;
        if (!GameState || !GameState->GetPlayerStructures(Total, Current))
        {
            return;
        }

        // The zone theme comes with the world record (OnInitWorldRecordComplete, 1.8 0x591340), before the world is
        // initialized; it is looked for until it is there.
        if (Watch->TemplateId.empty())
        {
            Watch->TemplateId = OutpostTemplateId(GameState);
            if (Watch->TemplateId.empty())
            {
                if (!Watch->bWarnedNoTheme && Now - Watch->Started >= ThemeWait)
                {
                    Watch->bWarnedNoTheme = true;
                    UE_LOG(LogFortOutpost, Warning, TEXT("KeepStructureLimit: %s's zone theme names no storm shield, so it keeps the game's structure limit (%d)"), *Outpost->GetName(), Current);
                }
                return;
            }
        }
        if (!Watch->bAsking && !Watch->bUnsupported && Now >= Watch->NextAsk)
        {
            Watch->NextAsk = Now + AskInterval;
            Ask(Watch);
        }

        if (Watch->Limit > 0 && Current != Watch->Limit)
        {
            // What the game had: its constructor's 3000, or the owner's homebase Manor_StructureCap if
            // ApplyStructureLimit (HandleMatchHasStarted, 0x58AF90) found one. The admin's limit wins over both.
            Watch->GameLimit = Current;
            GameState->SetMaxPlayerStructures(Watch->Limit);
            UE_LOG(LogFortOutpost, Log, TEXT("KeepStructureLimit: %s holds %d structures (the backend's, was %d), %d placed"), Watch->TemplateId.c_str(), Watch->Limit, Current, Total);
        }
        else if (Watch->Limit == 0 && Watch->GameLimit >= 0)
        {
            if (Current != Watch->GameLimit)
            {
                GameState->SetMaxPlayerStructures(Watch->GameLimit);
                UE_LOG(LogFortOutpost, Log, TEXT("KeepStructureLimit: %s is back at the game's structure limit (%d), %d placed"), Watch->TemplateId.c_str(), Watch->GameLimit, Total);
            }
            Watch->GameLimit = -1;
        }
    }

    void WatchStructureLimit(FWeakObjectPtr GameMode, std::shared_ptr<FStructureLimitWatch> State)
    {
        GameThread::Enqueue([GameMode, State]()
        {
            // Ends with the zone.
            AFortGameModeOutpost* Outpost = static_cast<AFortGameModeOutpost*>(GameMode.Get());
            if (!Outpost)
            {
                return;
            }
            const auto Now = std::chrono::steady_clock::now();
            if (Now >= State->NextCheck)
            {
                State->NextCheck = Now + std::chrono::seconds(1);
                Check(Outpost, State, Now);
            }
            WatchStructureLimit(GameMode, State);
        });
    }
}

void AFortGameModeOutpost::KeepStructureLimit()
{
#if WITH_SERVER_CODE
    // Core-Backend #115: a Storm Shield's structure limit from the admin panel, for all of them or each its own. Building
    // is refused once the game state's TotalPlayerStructures reaches MaxPlayerStructures (CoreV2 #128). The game sets
    // that to 3000 (AFortGameStateOutpost's constructor, 1.8 0x5AEAF0), or, when the match starts, to the owner's homebase
    // FortHomebaseSet.Manor_StructureCap if it is above 0 (ApplyStructureLimit, 0x57C440). Nothing the backend serves
    // sets that attribute's number: only game data effects raise it, by fixed amounts (GE_Outpost_Static_Core_1 +3000,
    // which no OutpostCore_PvE_0N's StaticGameplayEffectsByLevel names), and it is the owner's account's, the same in
    // every Storm Shield. So the server asks the backend for its Storm Shield's limit, by the zone theme's
    // CloudSaveItemDefinition (Outpost:outpostcore_pve_01..04: Stonewood to Twine Peaks), and keeps the game state at it,
    // checked once a second and asked again every 30 seconds. Without one the game's own limit stays.
    WatchStructureLimit(FWeakObjectPtr(this), std::make_shared<FStructureLimitWatch>());
#endif
}
