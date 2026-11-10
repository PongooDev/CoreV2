#include "pch.h"
#include "DeployableBases/FortDeployableBaseManager.h"
#include "Engine/World.h"
#include "Finder.h"
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

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <vector>
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
        TStructArray<FDeployableBaseInstance>& Plots = Array.DeployableBases;
        TStructArray<FPendingDeployableBaseUser>& Pending = Manager->PendingUsers;
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

    // Before AFortGameModeDeployableBase's own FinishWorldInitialization, which hands over to the manager
    // (FinishWorldInitialization below).
    const int32_t Placements = PlotPlacementActors(World);
    if (!Manager)
    {
        Log::Error(L"Horde: {} has no deployable base manager at world initialization ({} plot placement actor(s))", GameMode->GetName().ToStdString(), Placements);
    }
    else
    {
        FDeployableBaseArray& Array = Manager->DeployableBaseArray;
        TStructArray<FDeployableBaseInstance>& Plots = Array.DeployableBases;
        TStructArray<FPendingDeployableBaseUser>& Pending = Manager->PendingUsers;
        const int32_t Expected = Manager->NumExpectedPlots;
        Log::Info(L"Horde: {} finishing world initialization: manager {}, item {}, plot class {}, unlocks {}, {} plot(s) of {} expected, {} PlacementActor.DeployableBasePlot placement actor(s), {} player(s) waiting",
            GameMode->GetName().ToStdString(), Manager->GetName().ToStdString(), NameOf(static_cast<UObject*>(Manager->DeployableBaseItemDefinition)),
            NameOf(static_cast<UClass*>(Manager->DeployableBasePlotClass)), NameOf(static_cast<UFortDeployableBaseSkillTreeUnlocks*>(Manager->SupportedSkillTreeBasedUnlocks)),
            Plots.Num(), Expected, Placements, Pending.Num());
    }

    auto State = std::make_shared<FHordeWatch>();
    State->GameMode = GameMode;
    Watch(State);
#endif
}

namespace
{
    // The manager's world initialization (5.0 CL-4204761 0xDCCC80), which AFortGameModeDeployableBase's
    // FinishWorldInitialization (0xDF33F0: GetCurrent, then a tail jump) runs, and the build's natives its successful
    // path calls: OnDeployableBasePlotsSpawned's broadcast (lea rcx, [this+OnDeployableBasePlotsSpawned]; xor edx, edx;
    // call), then, for each pending player, giving them a plot (0xDBC350), finishing their join (0xDAFF60) or kicking
    // them with "Operation: Block Party failed to materialize" (0xDC7940).
    struct FWorldInitializationNatives
    {
        uintptr_t WorldInitialization = 0;
        uint32_t PlotsSpawnedOffset = 0;
        void (*BroadcastPlotsSpawned)(void* Delegate, void* Params) = nullptr;
        bool (*AssignPlot)(AFortDeployableBaseManager* Manager, FPendingDeployableBaseUser* PendingUser) = nullptr;
        void (*CompletePendingUser)(AFortDeployableBaseManager* Manager, int32_t Index) = nullptr;
        void (*KickPendingUser)(AFortDeployableBaseManager* Manager, int32_t Index) = nullptr;
    };

    FWorldInitializationNatives WorldInitializationNatives;

    uint8_t CodeByte(uintptr_t Address)
    {
        return *reinterpret_cast<const uint8_t*>(Address);
    }

    bool ContainsBytes(uintptr_t Begin, uintptr_t End, std::initializer_list<int32_t> Pattern)
    {
        for (uintptr_t Site = Begin; Site + Pattern.size() <= End; ++Site)
        {
            size_t Matched = 0;
            for (const int32_t Expected : Pattern)
            {
                if (Expected >= 0 && CodeByte(Site + Matched) != static_cast<uint8_t>(Expected))
                {
                    break;
                }
                ++Matched;
            }
            if (Matched == Pattern.size())
            {
                return true;
            }
        }
        return false;
    }

    bool ResolveWorldInitializationNatives(void* GameModeFinish)
    {
        static std::unordered_map<void*, FWorldInitializationNatives> Resolved;
        if (const auto Known = Resolved.find(GameModeFinish); Known != Resolved.end())
        {
            WorldInitializationNatives = Known->second;
            return WorldInitializationNatives.KickPendingUser != nullptr;
        }
        FWorldInitializationNatives& Natives = Resolved[GameModeFinish];

        // The game mode's FinishWorldInitialization: a short call to GetCurrent, then a jmp to the manager's.
        const uintptr_t Finish = reinterpret_cast<uintptr_t>(GameModeFinish);
        const uintptr_t FinishEnd = Finish ? Finder::FunctionEnd(Finish) : 0;
        for (uintptr_t Site = Finish; FinishEnd > Finish && FinishEnd - Finish <= 0x40 && Site + 5 <= FinishEnd && !Natives.WorldInitialization; ++Site)
        {
            const uintptr_t Target = CodeByte(Site) == 0xE9 ? Finder::RelativeTarget(Site, 1, 5) : 0;
            Natives.WorldInitialization = Target && Finder::FunctionStart(Target) == Target ? Target : 0;
        }
        const uintptr_t Begin = Natives.WorldInitialization;
        const uintptr_t End = Begin ? Finder::FunctionEnd(Begin) : 0;
        // bWorldInitialized: or dword ptr [this+550h], 2.
        if (!End || !ContainsBytes(Begin, End, { 0x83, -1, 0x50, 0x05, 0x00, 0x00, 0x02 }))
        {
            Log::Error(L"Finder: AFortDeployableBaseManager world initialization not found (game mode's FinishWorldInitialization +0x{:X})", Finish ? Finish - Finder::ImageBase() : 0);
            Natives = {};
            return false;
        }

        // lea rcx, [reg+disp32]; [xor edx, edx;] call: the plots spawned broadcast.
        uintptr_t BroadcastSite = 0;
        for (uintptr_t Site = Begin; Site + 12 <= End && !BroadcastSite; ++Site)
        {
            if (CodeByte(Site) != 0x48 || CodeByte(Site + 1) != 0x8D || (CodeByte(Site + 2) & 0xF8) != 0x88)
            {
                continue;
            }
            const uintptr_t Next = Site + 7 + (CodeByte(Site + 7) == 0x33 && CodeByte(Site + 8) == 0xD2 ? 2 : 0);
            if (CodeByte(Next) == 0xE8)
            {
                Natives.PlotsSpawnedOffset = *reinterpret_cast<const uint32_t*>(Site + 3);
                BroadcastSite = Next;
            }
        }
        // Only where the plot spawn before the check is stripped: a call to a stub with the world.
        bool bStripped = false;
        for (const auto& [Site, Target] : BroadcastSite ? Finder::DirectCalls(Begin, BroadcastSite) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            bStripped = bStripped || Finder::IsStubFunction(Target);
        }
        if (!bStripped)
        {
            Log::Info(L"Finder: AFortDeployableBaseManager world initialization +0x{:X} isn't stripped, the build's own runs", Begin - Finder::ImageBase());
            Natives = {};
            return false;
        }
        std::vector<uintptr_t> Targets;
        for (const auto& [Site, Target] : BroadcastSite ? Finder::DirectCalls(BroadcastSite, End) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (std::find(Targets.begin(), Targets.end(), Target) == Targets.end())
            {
                Targets.push_back(Target);
            }
        }
        // Targets[0] is the broadcast; the pending players' loop calls the other three in this order.
        const uintptr_t Kick = Targets.size() >= 4 ? Targets[3] : 0;
        const uintptr_t KickEnd = Kick ? Finder::FunctionEnd(Kick) : 0;
        const std::vector<uintptr_t> KickNames = Kick ? Finder::StringRefs(L"KickPendingPlayerError") : std::vector<uintptr_t>();
        const bool bKickNamed = std::any_of(KickNames.begin(), KickNames.end(), [Kick, KickEnd](uintptr_t Reference) { return Reference >= Kick && Reference < KickEnd; });
        const UProperty* Delegate = FindProperty(AFortDeployableBaseManager::StaticClass()->GetDefaultObject(false), L"OnDeployableBasePlotsSpawned", CASTCLASS_UMulticastDelegateProperty);
        if (!Delegate || Natives.PlotsSpawnedOffset != static_cast<uint32_t>(Delegate->GetOffset_ForInternal()) || !bKickNamed)
        {
            Log::Error(L"Finder: AFortDeployableBaseManager world initialization natives not decoded (+0x{:X}, delegate +0x{:X}, {} callee(s), kick named {})", Begin - Finder::ImageBase(),
                Natives.PlotsSpawnedOffset, Targets.size(), bKickNamed);
            Natives = {};
            return false;
        }
        Natives.BroadcastPlotsSpawned = reinterpret_cast<decltype(Natives.BroadcastPlotsSpawned)>(Targets[0]);
        Natives.AssignPlot = reinterpret_cast<decltype(Natives.AssignPlot)>(Targets[1]);
        Natives.CompletePendingUser = reinterpret_cast<decltype(Natives.CompletePendingUser)>(Targets[2]);
        Natives.KickPendingUser = reinterpret_cast<decltype(Natives.KickPendingUser)>(Kick);
        Log::Info(L"Finder: AFortDeployableBaseManager world initialization +0x{:X} (plots spawned broadcast +0x{:X}, assign plot +0x{:X}, complete +0x{:X}, kick +0x{:X})",
            Begin - Finder::ImageBase(), Targets[0] - Finder::ImageBase(), Targets[1] - Finder::ImageBase(), Targets[2] - Finder::ImageBase(), Kick - Finder::ImageBase());
        WorldInitializationNatives = Natives;
        return true;
    }
}

int32_t AFortDeployableBaseManager::SpawnDeployableBasePlots(UWorld* World)
{
    UClass* PlotClass = DeployableBasePlotClass;
    FDeployableBaseArray& Array = DeployableBaseArray;
    TStructArray<FDeployableBaseInstance>& Plots = Array.DeployableBases;
    const FGameplayTag PlotTag{ FName(L"PlacementActor.DeployableBasePlot", FNAME_Find) };
    int32_t Spawned = 0;
    for (TObjectIterator<AFortPlacementActorHorde> It; World && PlotClass && PlotTag.IsValid() && It; ++It)
    {
        const FGameplayTagContainer& Tags = It->StaticGameplayTags;
        if (It->GetWorld() != World || It->IsPendingKill() || !Tags.HasTag(PlotTag))
        {
            continue;
        }
        const FTransform Transform = It->GetTransform();
        FActorSpawnParameters Parameters;
        Parameters.Owner = this;
        Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Plot = World->SpawnActor(PlotClass, &Transform, Parameters);
        if (!Plot)
        {
            Log::Error(L"Horde: {} failed to spawn at {}", PlotClass->GetName().ToStdString(), It->GetName().ToStdString());
            continue;
        }
        FDeployableBaseInstance& Instance = Plots.AddDefaulted();
        Instance.DeployableBase = reinterpret_cast<ADeployableBasePlot*>(Plot);
        Array.MarkItemDirty(Instance);
        ++Spawned;
        Log::Info(L"Horde: plot {} spawned at {}", Plot->GetName().ToStdString(), It->GetName().ToStdString());
    }
    return Spawned;
}

void AFortDeployableBaseManager::OnWorldInitialized()
{
    FDeployableBaseArray& Array = DeployableBaseArray;
    TStructArray<FDeployableBaseInstance>& Plots = Array.DeployableBases;
    if (Plots.Num() == 0)
    {
        SpawnDeployableBasePlots(GetWorld());
    }
    TStructArray<FPendingDeployableBaseUser>& Pending = PendingUsers;
    Log::Info(L"Horde: deployable base manager world initialization: {} plot(s) of {} expected, {} player(s) waiting", Plots.Num(), static_cast<int32_t>(NumExpectedPlots), Pending.Num());

    WorldInitializationNatives.BroadcastPlotsSpawned(reinterpret_cast<uint8_t*>(this) + WorldInitializationNatives.PlotsSpawnedOffset, nullptr);
    for (int32_t Index = Pending.Num() - 1; Index >= 0; --Index)
    {
        if (static_cast<ADeployableBasePlot*>(Pending[Index].BasePlot))
        {
            continue;
        }
        if (WorldInitializationNatives.AssignPlot(this, &Pending[Index]))
        {
            if (static_cast<UObject*>(Pending[Index].BaseRecord) && static_cast<ADeployableBasePlot*>(Pending[Index].BasePlot))
            {
                Log::Info(L"Horde: a waiting player got their plot");
                WorldInitializationNatives.CompletePendingUser(this, Index);
            }
        }
        else
        {
            Log::Warning(L"Horde: no free plot for a waiting player, who is kicked");
            WorldInitializationNatives.KickPendingUser(this, Index);
        }
    }
    bWorldInitialized = true;
}

bool AFortDeployableBaseManager::ResolveWorldInitialization(void* GameModeFinish)
{
    return ResolveWorldInitializationNatives(GameModeFinish);
}

bool AFortDeployableBaseManager::FinishWorldInitialization(AFortGameModeZone* GameMode, void* GameModeFinish)
{
#if WITH_SERVER_CODE
    UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
    bool bDeployableBaseZone = false;
    AFortDeployableBaseManager* Manager = ManagerOf(World, bDeployableBaseZone);
    UClass* PlotBase = Manager ? FindObject<UClass>(nullptr, L"/Script/FortniteGame.DeployableBasePlot") : nullptr;
    UClass* PlotClass = Manager ? static_cast<UClass*>(Manager->DeployableBasePlotClass) : nullptr;
    if (!PlotBase || !PlotClass || !PlotClass->IsChildOf(PlotBase) || !ResolveWorldInitializationNatives(GameModeFinish))
    {
        return false;
    }
    Manager->OnWorldInitialized();
    return true;
#else
    return false;
#endif
}
