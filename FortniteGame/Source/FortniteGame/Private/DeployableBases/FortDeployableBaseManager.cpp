#include "pch.h"
#include "DeployableBases/FortDeployableBaseManager.h"
#include "Engine/World.h"
#include "FortGameModeZone.h"
#include "GameThread.h"
#include "Log.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionInfo.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <unordered_map>

AFortDeployableBaseManager* AFortDeployableBaseManager::GetCurrent(UObject* WorldContextObject)
{
    Reflection::FParams Params(StaticFunction_GetCurrent());
    Params.Set(L"WorldContextObject", WorldContextObject);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<AFortDeployableBaseManager*>(L"ReturnValue") : nullptr;
}

ADeployableBasePlot* AFortDeployableBaseManager::GetDeployableBasePlotFromLocation(const FVector& Location) const
{
    Reflection::FParams Params(StaticFunction_GetDeployableBasePlotFromLocation());
    Params.Set(L"Location", Location);
    return Params.Call(const_cast<AFortDeployableBaseManager*>(this)) ? Params.Take<ADeployableBasePlot*>(L"ReturnValue") : nullptr;
}

namespace
{
    class AFortPlacementActorHorde : public AActor
    {
        DefineUnrealClass(AFortPlacementActor, FortniteGame)

    public:
        DefineUProperty(FGameplayTagContainer, StaticGameplayTags)
    };

    class AFortMissionStormShieldHorde : public AActor
    {
        DefineUnrealClass(AFortMissionStormShield, FortniteGame)

    public:
        DefineUProperty(float, Radius)
        DefineUProperty(float, InitialRadius)
    };

    const UProperty* FindProperty(const UObject* Object, const wchar_t* Name, EClassCastFlags Kind)
    {
        const UClass* Class = Object ? Object->GetClass() : nullptr;
        const UProperty* Property = Class ? Class->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property && Property->GetClass()->HasAnyCastFlag(Kind) ? Property : nullptr;
    }

    std::optional<int32_t> IntOf(const UObject* Object, const wchar_t* Name)
    {
        const UProperty* Property = FindProperty(Object, Name, CASTCLASS_UIntProperty);
        return Property ? std::optional<int32_t>(*Property->ContainerPtrToValuePtr<int32_t>(Object)) : std::nullopt;
    }

    std::optional<bool> BoolOf(const UObject* Object, const wchar_t* Name)
    {
        const UBoolProperty* Property = static_cast<const UBoolProperty*>(FindProperty(Object, Name, CASTCLASS_UBoolProperty));
        return Property ? std::optional<bool>(Property->GetPropertyValue_InContainer(Object)) : std::nullopt;
    }

    // The game state's AFortGameStateDeployableBase::DeployableBaseManager, or null outside a deployable base zone.
    AFortDeployableBaseManager* ManagerOf(UWorld* World, bool& bOutDeployableBaseZone)
    {
        UObject* GameState = World ? reinterpret_cast<UObject*>(World->GetGameState()) : nullptr;
        const UProperty* Property = FindProperty(GameState, L"DeployableBaseManager", CASTCLASS_UObjectProperty);
        bOutDeployableBaseZone = Property != nullptr;
        return Property ? *Property->ContainerPtrToValuePtr<AFortDeployableBaseManager*>(GameState) : nullptr;
    }

    std::wstring NameOf(const UObject* Object)
    {
        return Object ? Object->GetName().ToStdString() : std::wstring(L"none");
    }

    int32_t PlotPlacementActors(UWorld* World)
    {
        const FGameplayTag PlotTag{ FName(L"PlacementActor.DeployableBasePlot", FNAME_Find) };
        int32_t Count = 0;
        for (TObjectIterator<AFortPlacementActorHorde> It; PlotTag.IsValid() && It; ++It)
        {
            const FGameplayTagContainer& Tags = It->StaticGameplayTags;
            Count += It->GetWorld() == World && Tags.HasTag(PlotTag) ? 1 : 0;
        }
        return Count;
    }

    struct FHordeWatch
    {
        FWeakObjectPtr GameMode;
        std::chrono::steady_clock::time_point NextCheck{};
        int32_t Plots = -1;
        int32_t PendingPlayers = -1;
        bool bWorldInitialized = false;
        FWeakObjectPtr Mission;
        int32_t Tier = 0;
        int32_t Wave = 0;
        bool bWaveRunning = false;
        uint8_t MissionStatus = 0xFF;
        std::unordered_map<const void*, float> ShieldRadii;
    };

    // The primary mission that runs tiered waves (GenMission_TieredWaves: Mission_ChallengeTheHorde) or the fort build.
    AFortMission* HordeMissionOf(UWorld* World)
    {
        for (TObjectIterator<AFortMission> It; It; ++It)
        {
            if (It->GetWorld() == World && !It->IsPendingKill() && It->IsPrimaryMission())
            {
                return *It;
            }
        }
        return nullptr;
    }

    void CheckManager(FHordeWatch& Watch, UWorld* World)
    {
        bool bDeployableBaseZone = false;
        AFortDeployableBaseManager* Manager = ManagerOf(World, bDeployableBaseZone);
        if (!Manager)
        {
            return;
        }
        FDeployableBaseArray& Array = Manager->DeployableBaseArray;
        const TArray<void*>& Plots = Array.DeployableBases;
        const TArray<void*>& Pending = Manager->PendingUsers;
        const bool bInitialized = Manager->bWorldInitialized;
        if (Plots.Num() != Watch.Plots || Pending.Num() != Watch.PendingPlayers || bInitialized != Watch.bWorldInitialized)
        {
            Log::Info(L"Horde: deployable base manager: {} plot(s) of {} expected, {} player(s) waiting for a plot, world initialized {}", Plots.Num(),
                static_cast<int32_t>(Manager->NumExpectedPlots), Pending.Num(), bInitialized);
            Watch.Plots = Plots.Num();
            Watch.PendingPlayers = Pending.Num();
            Watch.bWorldInitialized = bInitialized;
        }
    }

    void CheckMission(FHordeWatch& Watch, UWorld* World)
    {
        AFortMission* Mission = static_cast<AFortMission*>(Watch.Mission.Get());
        if (!Mission)
        {
            Mission = HordeMissionOf(World);
            Watch.Mission = Mission;
            if (!Mission)
            {
                return;
            }
            Log::Info(L"Horde: primary mission {} ({})", Mission->GetName().ToStdString(), NameOf(reinterpret_cast<UObject*>(static_cast<UFortMissionInfo*>(Mission->MissionInfo))));
        }

        const EFortMissionStatus Status = Mission->MissionStatus;
        if (static_cast<uint8_t>(Status) != Watch.MissionStatus)
        {
            Watch.MissionStatus = static_cast<uint8_t>(Status);
            if (Status == EFortMissionStatus::GetInProgress())
            {
                Log::Info(L"Horde: mission {} started", Mission->GetName().ToStdString());
            }
        }

        const std::optional<int32_t> Tier = IntOf(Mission, L"Tier");
        const std::optional<int32_t> Wave = IntOf(Mission, L"Wave");
        const std::optional<bool> bWaveRunning = BoolOf(Mission, L"bWaveRunning");
        if (!Tier || !Wave || !bWaveRunning)
        {
            return;
        }
        if (*Tier != Watch.Tier)
        {
            Log::Info(L"Horde: tier {} (was {})", *Tier, Watch.Tier);
        }
        if (*bWaveRunning && !Watch.bWaveRunning)
        {
            Log::Info(L"Horde: wave {} of tier {} started", *Wave, *Tier);
        }
        else if (!*bWaveRunning && Watch.bWaveRunning)
        {
            // GenMission_TieredWaves calls AFortMission::OnWaveCompleted(Tier, Wave, Result) as the wave ends; the build's
            // native sends each participant's quest manager a WaveComplete stat with Mission.Tier and Mission.Wave.
            Log::Info(L"Horde: wave {} of tier {} ended; AFortMission::OnWaveCompleted sends WaveComplete (Stat.Mission.Primary.Horde, Mission.Tier {}, Mission.Wave {})",
                Watch.Wave, Watch.Tier, Watch.Tier, Watch.Wave);
        }
        Watch.Tier = *Tier;
        Watch.Wave = *Wave;
        Watch.bWaveRunning = *bWaveRunning;
    }

    void CheckStormShields(FHordeWatch& Watch, UWorld* World)
    {
        for (TObjectIterator<AFortMissionStormShieldHorde> It; It; ++It)
        {
            if (It->GetWorld() != World || It->IsPendingKill())
            {
                continue;
            }
            const float InitialRadius = It->InitialRadius;
            auto [Known, bNew] = Watch.ShieldRadii.try_emplace(*It, InitialRadius);
            if (bNew || Known->second != InitialRadius)
            {
                Known->second = InitialRadius;
                Log::Info(L"Horde: storm shield {} reset to radius {:.0f} (now {:.0f}) for wave {} of tier {}", It->GetName().ToStdString(), InitialRadius,
                    static_cast<float>(It->Radius), Watch.Wave, Watch.Tier);
            }
        }
    }

    void Watch(std::shared_ptr<FHordeWatch> State)
    {
        GameThread::Enqueue([State]()
        {
            AFortGameModeZone* GameMode = static_cast<AFortGameModeZone*>(State->GameMode.Get());
            UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
            if (!World)
            {
                return;
            }
            const auto Now = std::chrono::steady_clock::now();
            if (Now >= State->NextCheck)
            {
                State->NextCheck = Now + std::chrono::seconds(1);
                CheckManager(*State, World);
                CheckMission(*State, World);
                CheckStormShields(*State, World);
            }
            Watch(State);
        });
    }
}

void AFortDeployableBaseManager::WatchHorde(AFortGameModeZone* GameMode)
{
#if WITH_SERVER_CODE
    UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
    bool bDeployableBaseZone = false;
    AFortDeployableBaseManager* Manager = ManagerOf(World, bDeployableBaseZone);
    if (!bDeployableBaseZone)
    {
        return;
    }

    // The game mode's FinishWorldInitialization (AFortGameModeDeployableBase's) hands over to the manager, which checks
    // that every expected plot is there and otherwise restarts the dedicated server (AFortGameSessionDedicated::Restart),
    // which drops the players waiting in the lobby (CoreV2 #353).
    const int32_t Placements = PlotPlacementActors(World);
    if (!Manager)
    {
        Log::Error(L"Horde: {} has no deployable base manager at world initialization ({} plot placement actor(s))", GameMode->GetName().ToStdString(), Placements);
    }
    else
    {
        FDeployableBaseArray& Array = Manager->DeployableBaseArray;
        const TArray<void*>& Plots = Array.DeployableBases;
        const TArray<void*>& Pending = Manager->PendingUsers;
        const int32_t Expected = Manager->NumExpectedPlots;
        Log::Info(L"Horde: {} world initialization: manager {}, item {}, plot class {}, unlocks {}, {} plot(s) of {} expected, {} PlacementActor.DeployableBasePlot placement actor(s), {} player(s) waiting",
            GameMode->GetName().ToStdString(), Manager->GetName().ToStdString(), NameOf(static_cast<UObject*>(Manager->DeployableBaseItemDefinition)),
            NameOf(static_cast<UClass*>(Manager->DeployableBasePlotClass)), NameOf(static_cast<UFortDeployableBaseSkillTreeUnlocks*>(Manager->SupportedSkillTreeBasedUnlocks)),
            Plots.Num(), Expected, Placements, Pending.Num());
        if (Expected > 0 || Plots.Num() != Expected)
        {
            Log::Warning(L"Horde: the manager's plots aren't all there ({} of {}); its world initialization restarts the server and drops the waiting players (CoreV2 #353)", Plots.Num(), Expected);
        }
    }

    auto State = std::make_shared<FHordeWatch>();
    State->GameMode = GameMode;
    Watch(State);
#endif
}
