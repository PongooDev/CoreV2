#include "pch.h"
#include "Diagnostics.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Log.h"
#include "ReflectionParams.h"
#include "UObject/AssetPtr.h"
#include "UObject/UObjectIterator.h"

#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace
{
    class ALobbyBeaconClient : public AActor
    {
        DefineUnrealClass(ALobbyBeaconClient, Lobby)

    public:
        DefineUProperty(UObject*, PlayerState)
    };

    class AFortLobbyBeaconPlayerState : public AActor
    {
        DefineUnrealClass(AFortLobbyBeaconPlayerState, FortniteGame)

    public:
        DefineUProperty(FString, HeroId)
        DefineUProperty(UObject*, HeroType)
        DefineUProperty(uint8_t, CharacterGender)
    };

    struct FFortAvailableMissionData
    {
        DefineUnrealStruct(FFortAvailableMissionData, FortniteGame)

    public:
        DefineUProperty(int32_t, TileIndex)
        DefineUProperty(TAssetSubclassOf<UObject>, MissionGenerator)
    };

    class UFortQuestItem : public UObject
    {
        DefineUnrealClass(UFortQuestItem, FortniteGame)
        DefineUFunction(GetQuestDefinitionBP)

    public:
        UObject* GetQuestDefinition()
        {
            Reflection::FParams Params(StaticFunction_GetQuestDefinitionBP());
            return Params.Call(this) ? Params.Take<UObject*>(L"ReturnValue") : nullptr;
        }
    };

    std::wstring NameOf(const UObject* Object)
    {
        return Object ? Object->GetFullName().ToStdString() : std::wstring(L"null");
    }

    std::wstring DescribeLobbyPlayer(UObject* PlayerState)
    {
        AFortLobbyBeaconPlayerState* FortPlayerState = PlayerState && PlayerState->IsA(AFortLobbyBeaconPlayerState::StaticClass()) ? static_cast<AFortLobbyBeaconPlayerState*>(PlayerState) : nullptr;
        if (!FortPlayerState)
        {
            return L"no FortLobbyBeaconPlayerState (" + NameOf(PlayerState) + L")";
        }
        FString& HeroId = FortPlayerState->HeroId;
        return FortPlayerState->GetName().ToStdString() + L" HeroId '" + HeroId.ToStdString() + L"' HeroType " + NameOf(FortPlayerState->HeroType) + L" CharacterGender " + std::to_wstring(static_cast<uint8_t>(FortPlayerState->CharacterGender));
    }

    void (*ServerSetHeroTypeOG)(ALobbyBeaconClient* This, const void* InUniqueId, UObject* InHeroType) = nullptr;
    void ServerSetHeroTypeHK(ALobbyBeaconClient* This, const void* InUniqueId, UObject* InHeroType)
    {
        Log::Info(L"Diagnostics: server received ServerSetHeroType {}", NameOf(InHeroType));
        ServerSetHeroTypeOG(This, InUniqueId, InHeroType);
        Log::Info(L"Diagnostics: server lobby player {}", DescribeLobbyPlayer(This ? static_cast<UObject*>(This->PlayerState) : nullptr));
    }

    void (*SetHeroTypeOG)(ALobbyBeaconClient* This, const void* InUniqueId, UObject* InHeroType) = nullptr;
    void SetHeroTypeHK(ALobbyBeaconClient* This, const void* InUniqueId, UObject* InHeroType)
    {
        Log::Info(L"Diagnostics: client sends SetHeroType {}", NameOf(InHeroType));
        SetHeroTypeOG(This, InUniqueId, InHeroType);
    }

    void WatchLobbyPlayers(int32_t TicksLeft)
    {
        GameThread::Enqueue([TicksLeft]()
        {
            if (TicksLeft > 0)
            {
                WatchLobbyPlayers(TicksLeft - 1);
                return;
            }

            static std::unordered_map<UObject*, std::wstring> Seen;
            for (TObjectIterator<AFortLobbyBeaconPlayerState> It; It; ++It)
            {
                const std::wstring Description = DescribeLobbyPlayer(*It);
                std::wstring& Last = Seen[*It];
                if (Last != Description)
                {
                    Last = Description;
                    Log::Info(L"Diagnostics: client lobby player {}", Description);
                }
            }
            WatchLobbyPlayers(60);
        });
    }
}

namespace
{
    // AFortTheaterMapViewer::GetMissionQuestValidity(Quest, Mission, Theater, MissionAlert, bForMissionAlert): how a mission
    // on the map serves a quest (EFortMissionQuestValidityResult Invalid, InvalidNotPlayable, ValidLinked,
    // ValidObjectiveCondition, ValidFallback). The quest log's Play Now shows only when some mission is valid (Core-Backend
    // #45); each quest's distinct answers are logged once.
    uint8_t (*MissionQuestValidityOG)(UObject* This, UObject* Quest, FFortAvailableMissionData* Mission, void* Theater, void* MissionAlert, bool bForMissionAlert) = nullptr;
    uint8_t MissionQuestValidityHK(UObject* This, UObject* Quest, FFortAvailableMissionData* Mission, void* Theater, void* MissionAlert, bool bForMissionAlert)
    {
        const uint8_t Result = MissionQuestValidityOG(This, Quest, Mission, Theater, MissionAlert, bForMissionAlert);
        static std::mutex Mutex;
        static std::set<std::tuple<UObject*, int32_t, uint8_t>> Logged;
        const int32_t TileIndex = Mission ? Mission->TileIndex : -1;
        std::lock_guard Lock(Mutex);
        if (Logged.emplace(Quest, TileIndex, Result).second)
        {
            const std::wstring Generator = Mission ? Mission->MissionGenerator.ToStringReference().ToString().ToStdString() : L"";
            UObject* Definition = Quest && Quest->IsA(UFortQuestItem::StaticClass()) ? static_cast<UFortQuestItem*>(Quest)->GetQuestDefinition() : nullptr;
            Log::Info(L"Diagnostics: quest {} tile {} generator {} alert {} -> validity {}", Definition ? Definition->GetName().ToStdString() : NameOf(Quest), TileIndex, Generator, MissionAlert != nullptr, Result);
        }
        return Result;
    }
}

void Diagnostics::HookQuestTiles()
{
    const auto Refs = Finder::StringRefs(L"AFortTheaterMapViewer::GetMissionQuestValidity");
    const uintptr_t Validity = Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    if (Validity)
    {
        Hooks::Detour(L"Diagnostics AFortTheaterMapViewer::GetMissionQuestValidity", reinterpret_cast<void*>(Validity), &MissionQuestValidityHK, &MissionQuestValidityOG);
    }
}

void Diagnostics::HookLobbyServer()
{
    const auto Refs = Finder::StringRefs(L"ServerSetHeroType %s");
    const uintptr_t ServerSetHeroType = Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    if (ServerSetHeroType)
    {
        Hooks::Detour(L"Diagnostics AFortLobbyBeaconClient::ServerSetHeroType_Implementation", reinterpret_cast<void*>(ServerSetHeroType), &ServerSetHeroTypeHK, &ServerSetHeroTypeOG);
    }
}

void Diagnostics::HookLobbyClient()
{
    const auto Refs = Finder::StringRefs(L"Not logged in when calling SetHeroType");
    const uintptr_t SetHeroType = Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    if (SetHeroType)
    {
        Hooks::Detour(L"Diagnostics AFortLobbyBeaconClient::SetHeroType", reinterpret_cast<void*>(SetHeroType), &SetHeroTypeHK, &SetHeroTypeOG);
    }
    WatchLobbyPlayers(60);
}
