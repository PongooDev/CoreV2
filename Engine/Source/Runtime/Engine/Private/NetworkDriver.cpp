#include "pch.h"
#include "Engine/NetDriver.h"
#include "Engine/ActorChannel.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/NetConnection.h"
#include "Engine/NetworkObjectList.h"
#include "Engine/PackageMapClient.h"
#include "Engine/ReplicationDriver.h"
#include "Engine/World.h"
#include "EngineLogs.h"
#include "Finder.h"
#include "GameFramework/GameNetworkManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "Hooks.h"
#include "Log.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/CommandLine.h"
#include "Misc/MemStack.h"
#include "Misc/Parse.h"
#include "Natives.h"
#include "Net/DataReplication.h"
#include "NetworkingDistanceConstants.h"
#include "Templates/Sorting.h"
#include "UObject/Class.h"

#include <vector>

static TAutoConsoleVariable<int32_t> CVarSetNetDormancyEnabled(
    TEXT("net.DormancyEnable"),
    1,
    TEXT("Enables Network Dormancy System for reducing CPU and bandwidth overhead of infrequently updated actors\n")
    TEXT("1 Enables network dormancy. 0 disables network dormancy."),
    ECVF_Default);

static TAutoConsoleVariable<int32_t> CVarNetDormancyValidate(
    TEXT("net.DormancyValidate"),
    0,
    TEXT("Validates that dormant actors do not change state while in a dormant state (on server only)")
    TEXT("0: Dont validate. 1: Validate on wake up. 2: Validate on each net update"),
    ECVF_Default);

bool UNetDriver::InitListen(FNetworkNotify* InNotify, FURL& ListenURL, bool bReuseAddressAndPort, FString& Error)
{
    if (Natives::UNetDriver_InitListenIndex < 0 && !Finder::ResolveInitListen())
    {
        return false;
    }

    using FInitListen = bool (*)(UNetDriver*, FNetworkNotify*, FURL&, bool, FString&);
    void** VTable = *reinterpret_cast<void***>(this);
    return reinterpret_cast<FInitListen>(VTable[Natives::UNetDriver_InitListenIndex])(this, InNotify, ListenURL, bReuseAddressAndPort, Error);
}

ENetMode UNetDriver::GetNetMode() const
{
    return static_cast<ENetMode>(Natives::UNetDriver_GetNetMode(this));
}

void UNetDriver::SetWorld(UWorld* InWorld)
{
    Natives::UNetDriver_SetWorld(this, InWorld);
}

bool UNetDriver::IsServer() const
{
    return CallVirtual<bool>(this, Natives::UNetDriver_IsServerIndex);
}

bool UNetDriver::IsLevelInitializedForActor(const AActor* InActor, const UNetConnection* InConnection) const
{
    return CallVirtual<bool>(this, Natives::UNetDriver_IsLevelInitializedForActorIndex, InActor, InConnection);
}

bool UNetDriver::IsAdaptiveNetUpdateFrequencyEnabled()
{
    return Natives::UNetDriver_IsAdaptiveNetUpdateFrequencyEnabled();
}

namespace
{
    std::vector<std::function<void()>> PreReplicationCallbacks;
}

void UNetDriver::AddPreReplicationCallback(std::function<void()> Callback)
{
    PreReplicationCallbacks.push_back(std::move(Callback));
}

void UNetDriver::TickFlush(float DeltaSeconds)
{
    if (IsServer() && ClientConnections.Num() > 0 && !ClientConnections[0]->InternalAck)
    {
#if WITH_SERVER_CODE
        RaiseClientNetSpeeds();
        for (const std::function<void()>& Callback : PreReplicationCallbacks)
        {
            Callback();
        }
        const int32_t Updated = ServerReplicateActors(DeltaSeconds);

        static int32_t LastUpdateCount = 0;
        if (Updated || LastUpdateCount)
        {
            UE_LOG(LogNetTraffic, Verbose, TEXT("%s replicated %d actors"), *GetDescription(), Updated);
        }
        LastUpdateCount = Updated;
        LogConnectionReplication();
#endif
    }

    TickFlushOG(this, DeltaSeconds);
}

void UNetDriver::RaiseClientNetSpeeds()
{
    const int32_t Rate = MaxClientRate;
    if (Rate < CoreMinClientRate || !UPlayer::HasCurrentNetSpeed())
    {
        return;
    }
    for (UNetConnection* Connection : ClientConnections)
    {
        if (Connection && Connection->CurrentNetSpeed < Rate)
        {
            Connection->CurrentNetSpeed = Rate;
        }
    }
}

namespace
{
    // Why a player's channel to an actor went, read from the actor and the connection the frame the channel is gone (the
    // engine keeps no close reason). A destroyed or torn-off actor isn't counted.
    enum class EChannelGoneReason : uint8_t
    {
        Dormant,
        LevelNotVisible,
        OutOfRange,
        InRange,
        Num,
    };

    const TCHAR* ChannelGoneReasonName(EChannelGoneReason Reason)
    {
        switch (Reason)
        {
        case EChannelGoneReason::Dormant: return TEXT("dormant");
        case EChannelGoneReason::LevelNotVisible: return TEXT("level not visible");
        case EChannelGoneReason::OutOfRange: return TEXT("out of range");
        default: return TEXT("in range");
        }
    }

    // An actor by its address and its weak pointer's hash (object index ^ serial number), so a new actor at a destroyed
    // one's address isn't taken for it reopening.
    struct FReplicatedActorKey
    {
        AActor* Actor = nullptr;
        uint32_t WeakHash = 0;

        bool operator==(const FReplicatedActorKey& Other) const { return Actor == Other.Actor && WeakHash == Other.WeakHash; }
        friend uint32_t GetTypeHash(const FReplicatedActorKey& Key) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(Key.Actor) >> 4) ^ Key.WeakHash; }
    };

    struct FClosedActorChannel
    {
        float Time = 0.0f;
        EChannelGoneReason Reason = EChannelGoneReason::InRange;
    };

    struct FReopenedClassStats
    {
        int32_t Count = 0;
        int32_t ByReason[static_cast<int32_t>(EChannelGoneReason::Num)] = {};
        FString Example;
    };

    struct FOpenActorChannel
    {
        TWeakObjectPtr<AActor> Actor;
        uint32_t SeenFrame = 0;
    };

    struct FConnectionReplicationStats
    {
        TMap<FReplicatedActorKey, FOpenActorChannel> OpenActors;
        TMap<FReplicatedActorKey, FClosedActorChannel> ClosedAt;
        TMap<UClass*, FReopenedClassStats> ReopenedByClass;
        // CoreV2 #694: channels opened in the window by the actor's class, to tell whether floor loot reaches a client.
        TMap<UClass*, int32_t> OpenedByClass;
        uint32_t Frame = 0;
        float WindowStart = -1.0f;
        int32_t Frames = 0;
        int32_t SaturatedFrames = 0;
        int32_t Opened = 0;
        int32_t Closed = 0;
        int32_t Reopened = 0;
        // The server view point the replication graph uses (FNetViewer: GetPlayerViewPoint, the camera manager's cache
        // that ServerUpdateCamera fills): frames it was over ViewPointOffDistance from this player's pawn, the
        // farthest, and of those frames the ones it was within that distance of another player's pawn.
        int32_t ViewFrames = 0;
        int32_t ViewOffOwnPawn = 0;
        float ViewMaxFromOwnPawn = 0.0f;
        int32_t ViewAtOtherPawn = 0;
        FString ViewAtOtherExample;
        // Frames the controller's camera manager had another owner, or was another connection's controller's too.
        int32_t CameraForeignOwner = 0;
        int32_t CameraShared = 0;
    };

    // CoreV2 #567: an island player's view point seemed to hold the other player's camera now and then.
    constexpr float ViewPointOffDistance = 5000.0f;

    class APlayerCameraManagerOwner : public UObject
    {
        DefineUnrealClass(APlayerCameraManager, Engine)
        DefineOptionalUProperty(APlayerController*, PCOwner)
    };

    struct FViewedPlayer
    {
        UNetConnection* Connection = nullptr;
        APlayerController* Controller = nullptr;
        APawn* Pawn = nullptr;
        UObject* CameraManager = nullptr;
    };

    FVector ActorLocation(AActor* Actor)
    {
        return Actor->GetTransform().GetLocation();
    }

    FString DescribeVector(const FVector& V)
    {
        return FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), V.X, V.Y, V.Z);
    }
    // By pointer: TMap's values are copied in, and the engine's containers here don't copy.
    TMap<UNetConnection*, FConnectionReplicationStats*> ConnectionReplicationStats;

    constexpr float ReplicationStatsWindow = 30.0f;
    // Wide enough for an actor reopened every 62 frames (2.07 s at 30 Hz), as #567's hub client saw.
    constexpr float ReopenWindow = 5.0f;

    FName LevelPackageName(const ULevel* Level)
    {
        const UObject* Package = Level;
        while (Package && Package->GetOuter())
        {
            Package = Package->GetOuter();
        }
        return Package ? Package->GetFName() : NAME_None;
    }

    bool IsStreamedLevel(const AActor* Actor, const ULevel* Level)
    {
        const UWorld* World = Actor->GetWorld();
        return Level && World && Level != World->PersistentLevel;
    }

    // ClientVisibleLevelNames holds the streamed levels' package names the client said it made visible
    // (APlayerController::ServerUpdateLevelVisibility).
    EChannelGoneReason ClassifyChannelGone(UNetConnection* Connection, AActor* Actor, float& OutDistance)
    {
        OutDistance = -1.0f;
        if (Natives::AActor_NetDormancy >= 0 && Actor->NetDormancy > DORM_Awake)
        {
            return EChannelGoneReason::Dormant;
        }
        ULevel* Level = Actor->GetLevel();
        if (IsStreamedLevel(Actor, Level) && Natives::UNetConnection_ClientVisibleLevelNames >= 0 && !Connection->IsClientLevelVisible(LevelPackageName(Level)))
        {
            return EChannelGoneReason::LevelNotVisible;
        }
        AActor* ViewTarget = Connection->ViewTarget;
        if (!ViewTarget)
        {
            return EChannelGoneReason::InRange;
        }
        const float DistanceSquared = FVector::DistSquared(Actor->GetTransform().GetLocation(), ViewTarget->GetTransform().GetLocation());
        OutDistance = FMath::Sqrt(DistanceSquared);
        const float CullDistanceSquared = Actor->NetCullDistanceSquared;
        return CullDistanceSquared > 0.0f && DistanceSquared > CullDistanceSquared ? EChannelGoneReason::OutOfRange : EChannelGoneReason::InRange;
    }
}

void UNetDriver::LogConnectionReplication()
{
#if WITH_SERVER_CODE
    if (Natives::UNetConnection_ActorChannels < 0 || Natives::UNetConnection_IsNetReadyIndex < 0)
    {
        return;
    }
    const float Now = Time;
    TArray<FViewedPlayer> Players;
    for (UNetConnection* Connection : ClientConnections)
    {
        APlayerController* Controller = Connection ? static_cast<APlayerController*>(Connection->PlayerController) : nullptr;
        if (Controller && Connection->State == USOCK_Open)
        {
            Players.Add(FViewedPlayer{ Connection, Controller, Controller->GetPawn(), reinterpret_cast<UObject*>(static_cast<APlayerCameraManager*>(Controller->PlayerCameraManager)) });
        }
    }
    TSet<UNetConnection*> Live;
    for (const FViewedPlayer& Player : Players)
    {
        UNetConnection* Connection = Player.Connection;
        APlayerController* Controller = Player.Controller;
        Live.Add(Connection);
        FConnectionReplicationStats*& StatsPtr = ConnectionReplicationStats.FindOrAdd(Connection);
        if (!StatsPtr)
        {
            StatsPtr = new FConnectionReplicationStats();
            StatsPtr->WindowStart = Now;
        }
        FConnectionReplicationStats& Stats = *StatsPtr;
        const uint32_t Frame = ++Stats.Frame;

        // The graph builds the same viewer for this connection each frame (its ServerReplicateActors, after setting
        // ViewTarget); the constructor reads the view target's root, so it needs one.
        if (Natives::FNetViewer_FNetViewer && Connection->ViewTarget && Player.Pawn)
        {
            const FNetViewer Viewer(Connection, 0.0f);
            const float FromOwnPawn = FVector::Dist(Viewer.ViewLocation, ActorLocation(Player.Pawn));
            ++Stats.ViewFrames;
            Stats.ViewMaxFromOwnPawn = FMath::Max(Stats.ViewMaxFromOwnPawn, FromOwnPawn);
            if (FromOwnPawn > ViewPointOffDistance)
            {
                ++Stats.ViewOffOwnPawn;
                for (const FViewedPlayer& Other : Players)
                {
                    if (Other.Connection == Connection || !Other.Pawn)
                    {
                        continue;
                    }
                    const float FromOther = FVector::Dist(Viewer.ViewLocation, ActorLocation(Other.Pawn));
                    if (FromOther <= ViewPointOffDistance)
                    {
                        ++Stats.ViewAtOtherPawn;
                        if (Stats.ViewAtOtherExample.IsEmpty())
                        {
                            Stats.ViewAtOtherExample = FString::Printf(TEXT("at %s %s, %.0f m from %s, %.0f m from its own pawn"), *DescribeVector(Viewer.ViewLocation),
                                *Other.Controller->GetName(), FromOther / 100.0f, *Other.Pawn->GetName(), FromOwnPawn / 100.0f);
                        }
                        break;
                    }
                }
            }
        }
        if (Player.CameraManager)
        {
            APlayerController* Owner = static_cast<APlayerCameraManagerOwner*>(Player.CameraManager)->PCOwner;
            Stats.CameraForeignOwner += Owner && Owner != Controller ? 1 : 0;
            for (const FViewedPlayer& Other : Players)
            {
                if (Other.Connection != Connection && Other.CameraManager == Player.CameraManager)
                {
                    ++Stats.CameraShared;
                    break;
                }
            }
        }

        for (auto& Pair : Connection->ActorChannels)
        {
            UActorChannel* Channel = Pair.Value;
            if (!Channel || !Channel->Actor)
            {
                continue;
            }
            const FReplicatedActorKey Key{ Channel->Actor, GetTypeHash(Pair.Key) };
            FOpenActorChannel& Open = Stats.OpenActors.FindOrAdd(Key);
            const bool bNew = Open.SeenFrame == 0;
            Open.Actor = Pair.Key;
            Open.SeenFrame = Frame;
            if (!bNew)
            {
                continue;
            }
            ++Stats.Opened;
            ++Stats.OpenedByClass.FindOrAdd(Channel->Actor->GetClass());
            const FClosedActorChannel* Closed = Stats.ClosedAt.Find(Key);
            if (!Closed || Now - Closed->Time > ReopenWindow)
            {
                continue;
            }
            ++Stats.Reopened;
            AActor* Actor = Channel->Actor;
            FReopenedClassStats& ClassStats = Stats.ReopenedByClass.FindOrAdd(Actor->GetClass());
            ++ClassStats.Count;
            ++ClassStats.ByReason[static_cast<int32_t>(Closed->Reason)];
            if (ClassStats.Example.IsEmpty())
            {
                float Distance = -1.0f;
                ClassifyChannelGone(Connection, Actor, Distance);
                ULevel* Level = Actor->GetLevel();
                const bool bStreamed = IsStreamedLevel(Actor, Level);
                const bool bLevelVisible = !bStreamed || Natives::UNetConnection_ClientVisibleLevelNames < 0 || Connection->IsClientLevelVisible(LevelPackageName(Level));
                const float CullDistanceSquared = Actor->NetCullDistanceSquared;
                ClassStats.Example = FString::Printf(TEXT("%s in %s (%s, visible to the client: %s), %.0f m away, cull %.0f m, closed while %s"),
                    *Actor->GetName(), *LevelPackageName(Level).ToString(), bStreamed ? TEXT("streamed") : TEXT("persistent"), bLevelVisible ? TEXT("yes") : TEXT("no"),
                    Distance / 100.0f, CullDistanceSquared > 0.0f ? FMath::Sqrt(CullDistanceSquared) / 100.0f : -1.0f, ChannelGoneReasonName(Closed->Reason));
            }
        }

        TArray<FReplicatedActorKey> Gone;
        for (const auto& Pair : Stats.OpenActors)
        {
            if (Pair.Value.SeenFrame != Frame)
            {
                Gone.Add(Pair.Key);
            }
        }
        for (const FReplicatedActorKey& Key : Gone)
        {
            AActor* Actor = Stats.OpenActors.FindChecked(Key).Actor.Get();
            Stats.OpenActors.Remove(Key);
            ++Stats.Closed;
            if (!Actor || Actor->bTearOff)
            {
                Stats.ClosedAt.Remove(Key);
                continue;
            }
            float Distance = -1.0f;
            Stats.ClosedAt.FindOrAdd(Key) = FClosedActorChannel{ Now, ClassifyChannelGone(Connection, Actor, Distance) };
        }
        ++Stats.Frames;
        Stats.SaturatedFrames += Connection->IsNetReady(false) ? 0 : 1;

        if (Now - Stats.WindowStart < ReplicationStatsWindow)
        {
            continue;
        }
        TArray<TPair<UClass*, const FReopenedClassStats*>> Classes;
        for (const auto& Pair : Stats.ReopenedByClass)
        {
            Classes.Add(TPair<UClass*, const FReopenedClassStats*>{ Pair.Key, &Pair.Value });
        }
        ::Sort(Classes.GetData(), Classes.Num(), [](const auto& A, const auto& B) { return A.Value->Count > B.Value->Count; });
        FString Top;
        for (int32_t i = 0; i < Classes.Num() && i < 6; ++i)
        {
            const FReopenedClassStats& ClassStats = *Classes[i].Value;
            FString Reasons;
            for (int32_t Reason = 0; Reason < static_cast<int32_t>(EChannelGoneReason::Num); ++Reason)
            {
                if (ClassStats.ByReason[Reason])
                {
                    Reasons += FString::Printf(TEXT("%s%s %d"), Reasons.IsEmpty() ? TEXT("") : TEXT(", "), ChannelGoneReasonName(static_cast<EChannelGoneReason>(Reason)), ClassStats.ByReason[Reason]);
                }
            }
            Top += FString::Printf(TEXT("%s%s %d [%s] (e.g. %s)"), i ? TEXT("; ") : TEXT(""), *Classes[i].Key->GetName(), ClassStats.Count, *Reasons, *ClassStats.Example);
        }
        TArray<TPair<UClass*, int32_t>> OpenedClasses;
        for (const auto& Pair : Stats.OpenedByClass)
        {
            OpenedClasses.Add(TPair<UClass*, int32_t>{ Pair.Key, Pair.Value });
        }
        ::Sort(OpenedClasses.GetData(), OpenedClasses.Num(), [](const auto& A, const auto& B) { return A.Value > B.Value; });
        FString OpenedTop;
        for (int32_t i = 0; i < OpenedClasses.Num() && i < 4; ++i)
        {
            OpenedTop += FString::Printf(TEXT("%s%s %d"), i ? TEXT(", ") : TEXT(" ("), *OpenedClasses[i].Key->GetName(), OpenedClasses[i].Value);
        }
        if (!OpenedTop.IsEmpty())
        {
            OpenedTop += TEXT(")");
        }
        AActor* ViewTarget = Connection->ViewTarget;
        FString ViewPoint = FString::Printf(TEXT("; view point over %.0f m from its pawn in %d of %d frames (farthest %.0f m), at another player's pawn in %d%s%s"),
            ViewPointOffDistance / 100.0f, Stats.ViewOffOwnPawn, Stats.ViewFrames, Stats.ViewMaxFromOwnPawn / 100.0f, Stats.ViewAtOtherPawn,
            Stats.ViewAtOtherExample.IsEmpty() ? TEXT("") : TEXT(", e.g. "), *Stats.ViewAtOtherExample);
        if (Natives::FNetViewer_FNetViewer && ViewTarget)
        {
            const FNetViewer Viewer(Connection, 0.0f);
            ViewPoint += FString::Printf(TEXT("; now %s"), *DescribeVector(Viewer.ViewLocation));
            for (const FViewedPlayer& Other : Players)
            {
                if (Other.Pawn)
                {
                    ViewPoint += FString::Printf(TEXT(", %.0f m from %s%s"), FVector::Dist(Viewer.ViewLocation, ActorLocation(Other.Pawn)) / 100.0f, *Other.Pawn->GetName(),
                        Other.Connection == Connection ? TEXT(" (its own)") : TEXT(""));
                }
            }
        }
        if (Player.CameraManager)
        {
            APlayerController* Owner = static_cast<APlayerCameraManagerOwner*>(Player.CameraManager)->PCOwner;
            ViewPoint += FString::Printf(TEXT("; camera %s, owner %s (another owner in %d frames, shared with another player in %d)"), *Player.CameraManager->GetName(),
                Owner == Controller ? TEXT("its own") : Owner ? *Owner->GetName() : TEXT("none"), Stats.CameraForeignOwner, Stats.CameraShared);
        }
        const FString Line = FString::Printf(TEXT("Net (#567): %s over %.0fs: %d frames, %d saturated; %d actor channels open, %d opened%s, %d closed, %d reopened within %.0fs%s%s; view target %s, net speed %d%s"),
            *Controller->GetName(), Now - Stats.WindowStart, Stats.Frames, Stats.SaturatedFrames, Stats.OpenActors.Num(), Stats.Opened, *OpenedTop, Stats.Closed, Stats.Reopened, ReopenWindow,
            Top.IsEmpty() ? TEXT("") : TEXT(": "), *Top, ViewTarget ? *ViewTarget->GetName() : TEXT("none"), UPlayer::HasCurrentNetSpeed() ? Connection->CurrentNetSpeed : -1, *ViewPoint);
        Log::Info(L"{}", Line.ToStdString());

        TArray<FReplicatedActorKey> Stale;
        for (const auto& Pair : Stats.ClosedAt)
        {
            if (Now - Pair.Value.Time > ReopenWindow)
            {
                Stale.Add(Pair.Key);
            }
        }
        for (const FReplicatedActorKey& Key : Stale)
        {
            Stats.ClosedAt.Remove(Key);
        }
        Stats.ReopenedByClass.Empty();
        Stats.OpenedByClass.Empty();
        Stats.WindowStart = Now;
        Stats.Frames = Stats.SaturatedFrames = Stats.Opened = Stats.Closed = Stats.Reopened = 0;
        Stats.ViewFrames = Stats.ViewOffOwnPawn = Stats.ViewAtOtherPawn = Stats.CameraForeignOwner = Stats.CameraShared = 0;
        Stats.ViewMaxFromOwnPawn = 0.0f;
        Stats.ViewAtOtherExample.Empty();
    }
    TArray<UNetConnection*> Dead;
    for (const auto& Pair : ConnectionReplicationStats)
    {
        if (!Live.Contains(Pair.Key))
        {
            Dead.Add(Pair.Key);
        }
    }
    for (UNetConnection* Connection : Dead)
    {
        delete ConnectionReplicationStats.FindChecked(Connection);
        ConnectionReplicationStats.Remove(Connection);
    }
#endif
}

void UNetDriver::FlushActorDormancy(AActor* Actor)
{
#if WITH_SERVER_CODE
    if (CVarSetNetDormancyEnabled.GetValueOnAnyThread() == 0)
    {
        return;
    }

    check(Actor);
    check(ServerConnection == nullptr);

    for (UNetConnection* NetConnection : ClientConnections)
    {
        if (NetConnection)
        {
            NetConnection->FlushDormancy(Actor);
        }
    }
#endif
}

namespace
{
    template <size_t Size>
    FActorDestructionInfo* FindDestructionInfoIn(UNetDriver::FDestroyedActorMap& Map, const FNetworkGUID& NetGUID)
    {
        using FSizedMap = TMap<FNetworkGUID, TActorDestructionInfoStorage<Size>>;
        static_assert(sizeof(FSizedMap) == sizeof(UNetDriver::FDestroyedActorMap));
        return reinterpret_cast<FActorDestructionInfo*>(reinterpret_cast<FSizedMap&>(Map).Find(NetGUID));
    }
}

FActorDestructionInfo* UNetDriver::FindDestructionInfo(const FNetworkGUID& NetGUID)
{
    if (Natives::UNetDriver_DestroyedStartupOrDormantActorsIsUnique)
    {
        // TUniquePtr<FActorDestructionInfo> holds just the pointer.
        using FUniqueMap = TMap<FNetworkGUID, FActorDestructionInfo*>;
        static_assert(sizeof(FUniqueMap) == sizeof(FDestroyedActorMap));
        FActorDestructionInfo** DestructionInfo = reinterpret_cast<FUniqueMap&>(DestroyedStartupOrDormantActors).Find(NetGUID);
        return DestructionInfo ? *DestructionInfo : nullptr;
    }

    // The finder only takes 4.16's 0x30 bytes or 4.19's 0x38.
    return Natives::FActorDestructionInfo_Size == 0x38 ? FindDestructionInfoIn<0x38>(DestroyedStartupOrDormantActors, NetGUID) : FindDestructionInfoIn<0x30>(DestroyedStartupOrDormantActors, NetGUID);
}

FNetViewer::FNetViewer(UNetConnection* InConnection, float DeltaSeconds)
{
    Natives::FNetViewer_FNetViewer(this, InConnection, DeltaSeconds);
}

FActorPriority::FActorPriority(UNetConnection* InConnection, UActorChannel* InChannel, FNetworkObjectInfo* InActorInfo, const TArray<FNetViewer>& Viewers, bool bLowBandwidth)
    : ActorInfo(InActorInfo), Channel(InChannel), DestructionInfo(nullptr)
{
    const float Time = Channel ? static_cast<float>(InConnection->Driver->Time - Channel->LastUpdateTime) : InConnection->Driver->SpawnPrioritySeconds;

    Priority = 0;
    for (const FNetViewer& Viewer : Viewers)
    {
        const float ViewerPriority = ActorInfo->Actor->GetNetPriority(Viewer.ViewLocation, Viewer.ViewDir, Viewer.InViewer, Viewer.ViewTarget, InChannel, Time, bLowBandwidth);
        Priority = FMath::Max<int32_t>(Priority, FMath::RoundToInt(65536.0f * ViewerPriority));
    }
}

FActorPriority::FActorPriority(UNetConnection* InConnection, FActorDestructionInfo* DestructInfo, const TArray<FNetViewer>& Viewers)
    : ActorInfo(nullptr), Channel(nullptr), DestructionInfo(DestructInfo)
{
    Priority = 0;
    for (const FNetViewer& Viewer : Viewers)
    {
        float Time = InConnection->Driver->SpawnPrioritySeconds;

        const FVector Dir = DestructionInfo->DestroyedPosition - Viewer.ViewLocation;
        const float DistSq = Dir.SizeSquared();

        if ((Viewer.ViewDir | Dir) < 0.f)
        {
            if (DistSq > NEARSIGHTTHRESHOLDSQUARED)
            {
                Time *= 0.2f;
            }
            else if (DistSq > CLOSEPROXIMITYSQUARED)
            {
                Time *= 0.4f;
            }
        }
        else if (DistSq > MEDSIGHTTHRESHOLDSQUARED)
        {
            Time *= 0.4f;
        }

        Priority = FMath::Max<int32_t>(Priority, static_cast<int32_t>(65536.0f * Time));
    }
}

#if WITH_SERVER_CODE
int32_t UNetDriver::ServerReplicateActors_PrepConnections(const float DeltaSeconds)
{
    int32_t NumClientsToTick = ClientConnections.Num();

    static const bool bForceClientTickingThrottle = FParse::Param(FCommandLine::Get(), TEXT("limitclientticks"));
    if (bForceClientTickingThrottle || GetNetMode() == NM_ListenServer)
    {
        static float DeltaTimeOverflow = 0.f;
        static const bool LanPlay = FParse::Param(FCommandLine::Get(), TEXT("lanplay"));

        const float ClientUpdatesThisFrame = GEngine->NetClientTicksPerSecond * (DeltaSeconds + DeltaTimeOverflow) * (LanPlay ? 2.f : 1.f);
        NumClientsToTick = FMath::Min<int32_t>(NumClientsToTick, FMath::TruncToInt(ClientUpdatesThisFrame));

        if (NumClientsToTick == 0)
        {
            DeltaTimeOverflow += DeltaSeconds;
            return 0;
        }
        DeltaTimeOverflow = 0.f;
    }

    bool bFoundReadyConnection = false;

    for (UNetConnection* Connection : ClientConnections)
    {
        check(Connection);
        check(Connection->State == USOCK_Pending || Connection->State == USOCK_Open || Connection->State == USOCK_Closed);

        AActor* OwningActor = Connection->OwningActor;
        const bool bReady = OwningActor && Connection->State == USOCK_Open && (Connection->Driver->Time - Connection->LastReceiveTime < 1.5f);

        if (!bReady)
        {
            Connection->ViewTarget = nullptr;
            for (UChildConnection* Child : Connection->Children)
            {
                Child->ViewTarget = nullptr;
            }
            continue;
        }

        check(World == OwningActor->GetWorld());
        bFoundReadyConnection = true;

        Connection->ViewTarget = Connection->PlayerController ? Connection->PlayerController->GetViewTarget() : OwningActor;

        for (UChildConnection* Child : Connection->Children)
        {
            APlayerController* ChildPlayerController = Child->PlayerController;
            Child->ViewTarget = ChildPlayerController ? ChildPlayerController->GetViewTarget() : nullptr;
        }
    }

    return bFoundReadyConnection ? NumClientsToTick : 0;
}

void UNetDriver::ServerReplicateActors_BuildConsiderList(TArray<FNetworkObjectInfo*>& OutConsiderList, const float ServerTickTime)
{
    UE_LOG(LogNetTraffic, Log, TEXT("ServerReplicateActors_BuildConsiderList, Building ConsiderList %4.2f"), World->GetTimeSeconds());

    int32_t NumInitiallyDormant = 0;

    const bool bUseAdapativeNetFrequency = IsAdaptiveNetUpdateFrequencyEnabled();

    TArray<AActor*> ActorsToRemove;

    for (const TSharedPtr<FNetworkObjectInfo>& ObjectInfo : GetNetworkObjectList().GetActiveObjects())
    {
        FNetworkObjectInfo* ActorInfo = ObjectInfo.Get();

        if (!ActorInfo->bPendingNetUpdate && World->TimeSeconds <= ActorInfo->NextUpdateTime)
        {
            continue;
        }

        AActor* Actor = ActorInfo->Actor;

        if (Actor->IsPendingKill() || Actor->GetRemoteRole() == ROLE_None)
        {
            ActorsToRemove.Add(Actor);
            continue;
        }

        if (Actor->GetNetDriverName() != NetDriverName)
        {
            UE_LOG(LogNetTraffic, Error, TEXT("Actor %s in wrong network actors list!"), *Actor->GetName());
            continue;
        }

        if (!Actor->IsActorInitialized())
        {
            continue;
        }

        ULevel* Level = Actor->GetLevel();
        if (Level->HasVisibilityChangeRequestPending() || Level->bIsAssociatingLevel)
        {
            continue;
        }

        if (Actor->NetDormancy == DORM_Initial && Actor->IsNetStartupActor())
        {
            NumInitiallyDormant++;
            ActorsToRemove.Add(Actor);
            continue;
        }

        checkSlow(World == Actor->GetWorld());

        if (ActorInfo->LastNetReplicateTime == 0)
        {
            ActorInfo->LastNetReplicateTime = World->TimeSeconds;
            ActorInfo->OptimalNetUpdateDelta = 1.0f / Actor->NetUpdateFrequency;
        }

        const float ScaleDownStartTime = 2.0f;
        const float ScaleDownTimeRange = 5.0f;

        const float LastReplicateDelta = static_cast<float>(World->TimeSeconds - ActorInfo->LastNetReplicateTime);

        if (LastReplicateDelta > ScaleDownStartTime)
        {
            if (Actor->MinNetUpdateFrequency == 0.0f)
            {
                Actor->MinNetUpdateFrequency = 2.0f;
            }

            const float MinOptimalDelta = 1.0f / Actor->NetUpdateFrequency;
            const float MaxOptimalDelta = FMath::Max(1.0f / Actor->MinNetUpdateFrequency, MinOptimalDelta);

            const float Alpha = FMath::Clamp((LastReplicateDelta - ScaleDownStartTime) / ScaleDownTimeRange, 0.0f, 1.0f);
            ActorInfo->OptimalNetUpdateDelta = FMath::Lerp(MinOptimalDelta, MaxOptimalDelta, Alpha);
        }

        if (!ActorInfo->bPendingNetUpdate)
        {
            UE_LOG(LogNetTraffic, Log, TEXT("actor %s requesting new net update, time: %2.3f"), *Actor->GetName(), World->TimeSeconds);

            const float NextUpdateDelta = bUseAdapativeNetFrequency ? ActorInfo->OptimalNetUpdateDelta : 1.0f / Actor->NetUpdateFrequency;

            ActorInfo->NextUpdateTime = World->TimeSeconds + FMath::SRand() * ServerTickTime + NextUpdateDelta;
            ActorInfo->LastNetUpdateTime = Time;
        }

        ActorInfo->bPendingNetUpdate = false;

        ensure(OutConsiderList.Num() < OutConsiderList.Max());
        OutConsiderList.Add(ActorInfo);

        Actor->CallPreReplication(this);
    }

    for (AActor* Actor : ActorsToRemove)
    {
        GetNetworkObjectList().Remove(Actor);
    }
}

static bool IsActorRelevantToConnection(const AActor* Actor, const TArray<FNetViewer>& ConnectionViewers)
{
    for (const FNetViewer& Viewer : ConnectionViewers)
    {
        if (Actor->IsNetRelevantFor(Viewer.InViewer, Viewer.ViewTarget, Viewer.ViewLocation))
        {
            return true;
        }
    }

    return false;
}

static UNetConnection* IsActorOwnedByAndRelevantToConnection(const AActor* Actor, const TArray<FNetViewer>& ConnectionViewers, bool& bOutHasNullViewTarget)
{
    const AActor* ActorOwner = Actor->GetNetOwner();

    bOutHasNullViewTarget = false;

    for (const FNetViewer& Viewer : ConnectionViewers)
    {
        UNetConnection* ViewerConnection = Viewer.Connection;
        AActor* ViewTarget = ViewerConnection->ViewTarget;
        APlayerController* ViewerController = ViewerConnection->PlayerController;

        if (!ViewTarget)
        {
            bOutHasNullViewTarget = true;
        }

        if (ActorOwner == ViewerController ||
            (ViewerController && ActorOwner == ViewerController->GetPawn()) ||
            (ViewTarget && ViewTarget->IsRelevancyOwnerFor(Actor, ActorOwner, ViewerConnection->OwningActor)))
        {
            return ViewerConnection;
        }
    }

    return nullptr;
}

static bool IsActorDormant(FNetworkObjectInfo* ActorInfo, const UNetConnection* Connection)
{
    return ActorInfo->DormantConnections.Contains(Connection);
}

static bool ShouldActorGoDormant(AActor* Actor, const TArray<FNetViewer>& ConnectionViewers, UActorChannel* Channel, const float Time, const bool bLowNetBandwidth)
{
    if (Actor->NetDormancy <= DORM_Awake || !Channel || Channel->bPendingDormancy || Channel->Dormant)
    {
        return false;
    }

    if (Actor->NetDormancy != DORM_DormantPartial)
    {
        return true;
    }

    for (const FNetViewer& Viewer : ConnectionViewers)
    {
        if (!Actor->GetNetDormancy(Viewer.ViewLocation, Viewer.ViewDir, Viewer.InViewer, Viewer.ViewTarget, Channel, Time, bLowNetBandwidth))
        {
            return false;
        }
    }

    return true;
}

int32_t UNetDriver::ServerReplicateActors_PrioritizeActors(UNetConnection* Connection, const TArray<FNetViewer>& ConnectionViewers, const TArray<FNetworkObjectInfo*>& ConsiderList, const bool bCPUSaturated, FActorPriority*& OutPriorityList, FActorPriority**& OutPriorityActors)
{
    NetTag++;
    // 1.9's FNetViewer no longer reads TickCount, and Core doesn't look for it there.
    if (Natives::UNetConnection_TickCount >= 0)
    {
        Connection->TickCount++;
    }

    for (AActor* Temporary : Connection->SentTemporaries)
    {
        Temporary->NetTag = NetTag;
    }

    check(World == Connection->OwningActor->GetWorld());

    int32_t FinalSortedCount = 0;

    const int32_t MaxSortedActors = ConsiderList.Num() + DestroyedStartupOrDormantActors.Num();
    if (MaxSortedActors > 0)
    {
        OutPriorityList = new (FMemStack::Get(), MaxSortedActors) FActorPriority;
        OutPriorityActors = new (FMemStack::Get(), MaxSortedActors) FActorPriority*;

        check(World == Connection->ViewTarget->GetWorld());

        AGameNetworkManager* const NetworkManager = World->NetworkManager;
        const bool bLowNetBandwidth = NetworkManager ? NetworkManager->IsInLowBandwidthMode() : false;

        for (FNetworkObjectInfo* ActorInfo : ConsiderList)
        {
            AActor* Actor = ActorInfo->Actor;

            UActorChannel* Channel = Connection->ActorChannels.FindRef(Actor);

            UNetConnection* PriorityConnection = Connection;

            if (Actor->bOnlyRelevantToOwner)
            {
                bool bHasNullViewTarget = false;
                PriorityConnection = IsActorOwnedByAndRelevantToConnection(Actor, ConnectionViewers, bHasNullViewTarget);

                if (!PriorityConnection)
                {
                    if (!bHasNullViewTarget && Channel && Time - Channel->RelevantTime >= RelevantTimeout)
                    {
                        Channel->Close(EChannelCloseReason::Relevancy);
                    }
                    continue;
                }
            }
            else if (CVarSetNetDormancyEnabled.GetValueOnGameThread() != 0)
            {
                if (IsActorDormant(ActorInfo, Connection))
                {
                    continue;
                }

                if (ShouldActorGoDormant(Actor, ConnectionViewers, Channel, Time, bLowNetBandwidth))
                {
                    Channel->StartBecomingDormant();
                }
            }

            if (!Channel && (!IsLevelInitializedForActor(Actor, Connection) || !IsActorRelevantToConnection(Actor, ConnectionViewers)))
            {
                continue;
            }

            if (Actor->NetTag == NetTag)
            {
                continue;
            }

            UE_LOG(LogNetTraffic, Log, TEXT("Consider %s alwaysrelevant %d frequency %f "), *Actor->GetName(), static_cast<bool>(Actor->bAlwaysRelevant), Actor->NetUpdateFrequency);

            Actor->NetTag = NetTag;

            OutPriorityList[FinalSortedCount] = FActorPriority(PriorityConnection, Channel, ActorInfo, ConnectionViewers, bLowNetBandwidth);
            OutPriorityActors[FinalSortedCount] = OutPriorityList + FinalSortedCount;
            FinalSortedCount++;

            if (DebugRelevantActors)
            {
                LastPrioritizedActors.Add(Actor);
            }
        }

        for (auto It = Connection->DestroyedStartupOrDormantActors.CreateIterator(); It; ++It)
        {
            FActorDestructionInfo* DInfo = FindDestructionInfo(*It);
            if (!DInfo)
            {
                continue;
            }
            OutPriorityList[FinalSortedCount] = FActorPriority(Connection, DInfo, ConnectionViewers);
            OutPriorityActors[FinalSortedCount] = OutPriorityList + FinalSortedCount;
            FinalSortedCount++;
        }

        Sort(OutPriorityActors, FinalSortedCount, FCompareFActorPriority());
    }

    UE_LOG(LogNetTraffic, Log, TEXT("ServerReplicateActors_PrioritizeActors: Potential %04i ConsiderList %03i FinalSortedCount %03i"), MaxSortedActors, ConsiderList.Num(), FinalSortedCount);

    return FinalSortedCount;
}

int32_t UNetDriver::ServerReplicateActors_ProcessPrioritizedActors(UNetConnection* Connection, const TArray<FNetViewer>& ConnectionViewers, FActorPriority** PriorityActors, const int32_t FinalSortedCount, int32_t& OutUpdated)
{
    if (!Connection->IsNetReady(0))
    {
        return 0;
    }

    for (int32_t j = 0; j < FinalSortedCount; j++)
    {
        FActorPriority& Entry = *PriorityActors[j];

        if (!Entry.ActorInfo && Entry.DestructionInfo)
        {
            FActorDestructionInfo& DestructionInfo = *Entry.DestructionInfo;
            if (DestructionInfo.StreamingLevelName != NAME_None && !Connection->IsClientLevelVisible(DestructionInfo.StreamingLevelName))
            {
                continue;
            }

            if (UActorChannel* Channel = static_cast<UActorChannel*>(Connection->CreateChannel(CHTYPE_Actor, 1)))
            {
                UE_LOG(LogNetTraffic, Log, TEXT("Server replicate actor creating destroy channel for NetGUID <%s,%s> Priority: %d"), *DestructionInfo.NetGUID.ToString(), *DestructionInfo.PathName, Entry.Priority);

                Channel->SetChannelActorForDestroy(&DestructionInfo);
                Connection->DestroyedStartupOrDormantActors.Remove(DestructionInfo.NetGUID);
            }
            continue;
        }

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
        static IConsoleVariable* DebugObjectCvar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.PackageMap.DebugObject"));
        static IConsoleVariable* DebugAllObjectsCvar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.PackageMap.DebugAll"));
        if (Entry.ActorInfo)
        {
            const bool bDebugThisObject = DebugObjectCvar && !DebugObjectCvar->GetString().IsEmpty() && Entry.ActorInfo->Actor->GetName().Contains(DebugObjectCvar->GetString());
            const bool bDebugAllObjects = DebugAllObjectsCvar && DebugAllObjectsCvar->GetInt() != 0;
            if (bDebugThisObject || bDebugAllObjects)
            {
                UE_LOG(LogNetPackageMap, Log, TEXT("Evaluating actor for replication %s"), *Entry.ActorInfo->Actor->GetName());
            }
        }
#endif

        UActorChannel* Channel = Entry.Channel;
        UE_LOG(LogNetTraffic, Log, TEXT(" Maybe Replicate %s"), *Entry.ActorInfo->Actor->GetName());

        if (Channel && !Channel->Actor)
        {
            continue;
        }

        AActor* Actor = Entry.ActorInfo->Actor;
        bool bIsRelevant = false;

        const bool bLevelInitializedForActor = IsLevelInitializedForActor(Actor, Connection);

        if (!bLevelInitializedForActor)
        {
            UE_LOG(LogNetTraffic, Log, TEXT("- Level not initialized for actor %s"), *Actor->GetName());
        }
        else if (!Actor->bTearOff && (!Channel || Time - Channel->RelevantTime > 1.f))
        {
            if (IsActorRelevantToConnection(Actor, ConnectionViewers))
            {
                bIsRelevant = true;
            }
            else if (DebugRelevantActors)
            {
                LastNonRelevantActors.Add(Actor);
            }
        }

        const bool bIsRecentlyRelevant = bIsRelevant || (Channel && Time - Channel->RelevantTime < RelevantTimeout);

        if (bIsRecentlyRelevant)
        {
            if (!Channel && GuidCache->SupportsObject(Actor->GetClass()) && GuidCache->SupportsObject(Actor->IsNetStartupActor() ? static_cast<UObject*>(Actor) : Actor->GetArchetype()))
            {
                if (bLevelInitializedForActor)
                {
                    Channel = static_cast<UActorChannel*>(Connection->CreateChannel(CHTYPE_Actor, 1));
                    if (Channel)
                    {
                        Channel->SetChannelActor(Actor);
                    }
                }
                else if (Actor->NetUpdateFrequency < 1.0f)
                {
                    UE_LOG(LogNetTraffic, Log, TEXT("Unable to replicate %s"), *Actor->GetName());
                    Entry.ActorInfo->NextUpdateTime = Actor->GetWorld()->TimeSeconds + 0.2f * FMath::FRand();
                }
            }

            if (Channel)
            {
                if (bIsRelevant)
                {
                    Channel->RelevantTime = Time + 0.5f * FMath::SRand();
                }

                if (Channel->IsNetReady(0))
                {
                    UE_LOG(LogNetTraffic, Log, TEXT("- Replicate %s. %d"), *Actor->GetName(), Entry.Priority);
                    if (DebugRelevantActors)
                    {
                        LastRelevantActors.Add(Actor);
                    }

                    if (Channel->ReplicateActor())
                    {
                        if (DebugRelevantActors)
                        {
                            LastSentActors.Add(Actor);
                        }

                        const float MinOptimalDelta = 1.0f / Actor->NetUpdateFrequency;
                        const float MaxOptimalDelta = FMath::Max(1.0f / Actor->MinNetUpdateFrequency, MinOptimalDelta);
                        const float DeltaBetweenReplications = static_cast<float>(World->TimeSeconds - Entry.ActorInfo->LastNetReplicateTime);

                        Entry.ActorInfo->OptimalNetUpdateDelta = FMath::Clamp(DeltaBetweenReplications * 0.7f, MinOptimalDelta, MaxOptimalDelta);
                        Entry.ActorInfo->LastNetReplicateTime = World->TimeSeconds;
                    }
                    OutUpdated++;
                }
                else
                {
                    UE_LOG(LogNetTraffic, Log, TEXT("- Channel saturated, forcing pending update for %s"), *Actor->GetName());
                    Actor->ForceNetUpdate();
                }

                if (!Connection->IsNetReady(0))
                {
                    return j;
                }
            }
        }

        if ((!bIsRecentlyRelevant || Actor->bTearOff) && Channel && (!bLevelInitializedForActor || !Actor->IsNetStartupActor()))
        {
            UE_LOG(LogNetTraffic, Log, TEXT("- Closing channel for no longer relevant actor %s"), *Actor->GetName());
            Channel->Close(Actor->bTearOff ? EChannelCloseReason::TearOff : EChannelCloseReason::Relevancy);
        }
    }

    return FinalSortedCount;
}
#endif

int32_t UNetDriver::ServerReplicateActors(float DeltaSeconds)
{
#if WITH_SERVER_CODE
    if (ClientConnections.Num() == 0)
    {
        return 0;
    }

    // UE 4.20: a net driver with a replication driver (3.4's Battle Royale has UFortReplicationGraph; 3.3 and Save the
    // World leave it off) hands it the whole job.
    if (HasReplicationDriver())
    {
        if (UReplicationDriver* Driver = ReplicationDriver)
        {
            return Driver->ServerReplicateActors(DeltaSeconds);
        }
    }

    check(World);

    int32_t Updated = 0;

    ReplicationFrame++;

    const int32_t NumClientsToTick = ServerReplicateActors_PrepConnections(DeltaSeconds);
    if (NumClientsToTick == 0)
    {
        return 0;
    }

    AWorldSettings* WorldSettings = World->GetWorldSettings();

    bool bCPUSaturated = false;
    float ServerTickTime = GEngine->GetMaxTickRate(DeltaSeconds);
    if (ServerTickTime == 0.f)
    {
        ServerTickTime = DeltaSeconds;
    }
    else
    {
        ServerTickTime = 1.f / ServerTickTime;
        bCPUSaturated = DeltaSeconds > 1.2f * ServerTickTime;
    }

    TArray<FNetworkObjectInfo*> ConsiderList;
    ConsiderList.Reserve(GetNetworkObjectList().GetActiveObjects().Num());

    ServerReplicateActors_BuildConsiderList(ConsiderList, ServerTickTime);

    FMemMark Mark(FMemStack::Get());

    for (int32_t i = 0; i < ClientConnections.Num(); i++)
    {
        UNetConnection* Connection = ClientConnections[i];
        check(Connection);

        if (CVarNetDormancyValidate.GetValueOnAnyThread() == 2)
        {
            for (auto It = Connection->DormantReplicatorMap.CreateIterator(); It; ++It)
            {
                FObjectReplicator& Replicator = *It.Value();
                if (Replicator.OwningChannel)
                {
                    Replicator.ValidateAgainstState(Replicator.OwningChannel->GetActor());
                }
            }
        }

        if (i >= NumClientsToTick)
        {
            for (FNetworkObjectInfo* ActorInfo : ConsiderList)
            {
                AActor* Actor = ActorInfo->Actor;
                if (!Actor || ActorInfo->bPendingNetUpdate)
                {
                    continue;
                }

                UActorChannel* Channel = Connection->ActorChannels.FindRef(Actor);
                if (Channel && Channel->LastUpdateTime < ActorInfo->LastNetUpdateTime)
                {
                    ActorInfo->bPendingNetUpdate = true;
                }
            }

            Connection->TimeSensitive = false;
            continue;
        }

        if (!Connection->ViewTarget)
        {
            continue;
        }

        TArray<FNetViewer>& ConnectionViewers = WorldSettings->ReplicationViewers;

        ConnectionViewers.Reset();
        new (ConnectionViewers) FNetViewer(Connection, DeltaSeconds);
        for (UChildConnection* Child : Connection->Children)
        {
            if (Child->ViewTarget)
            {
                new (ConnectionViewers) FNetViewer(Child, DeltaSeconds);
            }
        }

        if (Connection->PlayerController)
        {
            Connection->PlayerController->SendClientAdjustment();
        }

        for (UChildConnection* Child : Connection->Children)
        {
            if (Child->PlayerController)
            {
                Child->PlayerController->SendClientAdjustment();
            }
        }

        FMemMark RelevantActorMark(FMemStack::Get());

        FActorPriority* PriorityList = nullptr;
        FActorPriority** PriorityActors = nullptr;

        const int32_t FinalSortedCount = ServerReplicateActors_PrioritizeActors(Connection, ConnectionViewers, ConsiderList, bCPUSaturated, PriorityList, PriorityActors);
        const int32_t LastProcessedActor = ServerReplicateActors_ProcessPrioritizedActors(Connection, ConnectionViewers, PriorityActors, FinalSortedCount, Updated);

        for (int32_t k = LastProcessedActor; k < FinalSortedCount; k++)
        {
            FNetworkObjectInfo* ActorInfo = PriorityActors[k]->ActorInfo;
            if (!ActorInfo)
            {
                continue;
            }

            AActor* Actor = ActorInfo->Actor;
            UActorChannel* Channel = PriorityActors[k]->Channel;

            UE_LOG(LogNetTraffic, Verbose, TEXT("Saturated. %s"), *Actor->GetName());

            const bool bRecentlyRelevantChannel = Channel && Time - Channel->RelevantTime <= 1.f;
            if (!bRecentlyRelevantChannel && !IsActorRelevantToConnection(Actor, ConnectionViewers))
            {
                continue;
            }

            UE_LOG(LogNetTraffic, Log, TEXT(" Saturated. Mark %s NetUpdateTime to be checked for next tick"), *Actor->GetName());
            ActorInfo->bPendingNetUpdate = true;

            if (!bRecentlyRelevantChannel && Channel)
            {
                Channel->RelevantTime = Time + 0.5f * FMath::SRand();
            }
        }

        RelevantActorMark.Pop();

        ConnectionViewers.Reset();
    }

    if (NumClientsToTick < ClientConnections.Num())
    {
        for (int32_t NumConnectionsToMove = NumClientsToTick; NumConnectionsToMove > 0; NumConnectionsToMove--)
        {
            UNetConnection* Connection = ClientConnections[0];
            ClientConnections.RemoveAt(0, 1);
            ClientConnections.Add(Connection);
        }
    }

    Mark.Pop();

    if (DebugRelevantActors)
    {
        PrintDebugRelevantActors();
        LastPrioritizedActors.Empty();
        LastSentActors.Empty();
        LastRelevantActors.Empty();
        LastNonRelevantActors.Empty();

        DebugRelevantActors = false;
    }

    return Updated;
#else
    return 0;
#endif
}

void UNetDriver::PrintDebugRelevantActors()
{
    const auto AggregateAndPrint = [](TArray<TWeakObjectPtr<AActor>>& List, const FString& Label)
    {
        TMap<TWeakObjectPtr<UClass>, int32_t> ClassSummary;
        TMap<TWeakObjectPtr<UClass>, int32_t> SuperClassSummary;

        for (const TWeakObjectPtr<AActor>& Entry : List)
        {
            if (AActor* Actor = Entry.Get())
            {
                UClass* ActorClass = Actor->GetClass();
                ClassSummary.FindOrAdd(ActorClass)++;
                if (ActorClass->GetSuperStruct())
                {
                    SuperClassSummary.FindOrAdd(ActorClass->GetSuperClass())++;
                }
            }
        }

        const auto ByCount = [](int32_t A, int32_t B)
        {
            return A < B;
        };
        ClassSummary.ValueSort(ByCount);
        SuperClassSummary.ValueSort(ByCount);

        const auto PrintSummary = [](TMap<TWeakObjectPtr<UClass>, int32_t>& Summary)
        {
            for (auto It = Summary.CreateIterator(); It; ++It)
            {
                UClass* SummaryClass = It.Key().Get();
                UStruct* SuperStruct = SummaryClass->GetSuperStruct();
                UE_LOG(LogNet, Warning, TEXT("%4d - %s (%s)"), It.Value(), *SummaryClass->GetName(), SuperStruct ? *SuperStruct->GetName() : TEXT("NULL"));
            }
        };

        UE_LOG(LogNet, Warning, TEXT("------------------------------"));
        UE_LOG(LogNet, Warning, TEXT(" %s Class Summary"), *Label);
        UE_LOG(LogNet, Warning, TEXT("------------------------------"));
        PrintSummary(ClassSummary);

        UE_LOG(LogNet, Warning, TEXT("---------------------------------"));
        UE_LOG(LogNet, Warning, TEXT(" %s Parent Class Summary "), *Label);
        UE_LOG(LogNet, Warning, TEXT("------------------------------"));
        PrintSummary(SuperClassSummary);

        UE_LOG(LogNet, Warning, TEXT("---------------------------------"));
        UE_LOG(LogNet, Warning, TEXT(" %s Total: %d"), *Label, List.Num());
        UE_LOG(LogNet, Warning, TEXT("---------------------------------"));
    };

    AggregateAndPrint(LastPrioritizedActors, TEXT(" Prioritized Actor"));
    AggregateAndPrint(LastRelevantActors, TEXT(" Relevant Actor"));
    AggregateAndPrint(LastNonRelevantActors, TEXT(" NonRelevant Actor"));
    AggregateAndPrint(LastSentActors, TEXT(" Sent Actor"));

    UE_LOG(LogNet, Warning, TEXT("---------------------------------"));
    UE_LOG(LogNet, Warning, TEXT(" Num Connections: %d"), ClientConnections.Num());
    UE_LOG(LogNet, Warning, TEXT("---------------------------------"));
}

void UNetDriver::Hook()
{
    Hooks::Detour(L"UNetDriver::TickFlush", reinterpret_cast<void*>(Natives::UNetDriver_TickFlush), &TickFlushHK, &TickFlushOG);

    for (void** VTable : Sites::NetDriverVTables)
    {
        Hooks::SwapVTable(L"UNetDriver::ServerReplicateActors", VTable, Natives::UNetDriver_ServerReplicateActorsIndex, reinterpret_cast<void*>(&ServerReplicateActorsHK));
    }

    for (uint8_t* Call : Sites::FlushActorDormancyCalls)
    {
        Hooks::RedirectCall(L"UNetDriver::FlushActorDormancy", Call, reinterpret_cast<void*>(&FlushActorDormancyHK));
    }
}

void UNetDriver::TickFlushHK(UNetDriver* This, float DeltaSeconds)
{
    static const bool bReplicationResolved = Finder::ResolveReplicationLive();
    if (!bReplicationResolved)
    {
        TickFlushOG(This, DeltaSeconds);
        return;
    }

    This->TickFlush(DeltaSeconds);
}

int32_t UNetDriver::ServerReplicateActorsHK(UNetDriver* This, float DeltaSeconds)
{
    return This->ServerReplicateActors(DeltaSeconds);
}

void UNetDriver::FlushActorDormancyHK(UNetDriver* This, AActor* Actor)
{
    This->FlushActorDormancy(Actor);
}
