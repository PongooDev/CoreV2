#pragma once

#include "Containers/Array.h"
#include "Containers/EnumAsByte.h"
#include "CoreGlobals.h"
#include "Engine/EngineBaseTypes.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "ObjectLayout.h"
#include "UObject/NameTypes.h"
#include "UObject/UnrealNames.h"
#include "UObject/Object.h"

class ULevel;
class UNetDriver;
class UActorChannel;
class UWorld;
class USceneComponent;
class UActorComponent;

enum ENetRole
{
    ROLE_None,
    ROLE_SimulatedProxy,
    ROLE_AutonomousProxy,
    ROLE_Authority,
    ROLE_MAX,
};

enum ENetDormancy
{
    DORM_Never,
    DORM_Awake,
    DORM_DormantAll,
    DORM_DormantPartial,
    DORM_Initial,
    DORM_MAX,
};

class AActor : public UObject
{
    DefineUnrealClass(AActor, Engine)

public:
    DefineBitfieldUProperty(bNetTemporary)
    DefineBitfieldUProperty(bNetStartup)
    DefineBitfieldUProperty(bOnlyRelevantToOwner)
    DefineBitfieldUProperty(bAlwaysRelevant)
    DefineBitfieldUProperty(bTearOff)
    DefineBitfieldUProperty(bReplicates)
    DefineBitfieldUProperty(bAllowReceiveTickEventOnDedicatedServer)
    DefineUProperty(FName, NetDriverName)
    DefineUProperty(TEnumAsByte<ENetRole>, Role)
    DefineUProperty(TEnumAsByte<ENetRole>, RemoteRole)
    DefineUProperty(AActor*, Owner)
    DefineUProperty(int32_t, NetTag)
    DefineUProperty(float, NetUpdateFrequency)
    DefineUProperty(float, MinNetUpdateFrequency)
    DefineNativeMember(TEnumAsByte<ENetDormancy>, NetDormancy, AActor_NetDormancy)
    DefineNativeBitfield(bActorInitialized, AActor_bActorInitialized, Natives::AActor_bActorInitializedMask)
    DefineUProperty(USceneComponent*, RootComponent)
    DefineUFunction(GetTransform)
    DefineUFunction(GetActorBounds)
    DefineUFunction(K2_SetActorLocation)
    DefineUFunction(K2_GetActorRotation)
    DefineUFunction(K2_TeleportTo)
    DefineUFunction(SetActorScale3D)
    DefineUFunction(SetActorEnableCollision)
    DefineUFunction(K2_DestroyActor)
    DefineUFunction(GetComponentsByClass)

    ENetRole GetRemoteRole() const { return RemoteRole; }
    AActor* GetOwner() const { return Owner; }
    bool IsNetStartupActor() const { return bNetStartup; }
    bool IsActorInitialized() const { return bActorInitialized; }
    const FName& GetNetDriverName() const { return NetDriverName; }

    ULevel* GetLevel() const;
    UWorld* GetWorld() const;
    UNetDriver* GetNetDriver() const;

    ENetMode GetNetMode() const;
    bool IsNetMode(ENetMode Mode) const;

    FTransform GetTransform() const;
    USceneComponent* GetRootComponent() const { return RootComponent; }
    void GetActorBounds(bool bOnlyCollidingComponents, FVector& Origin, FVector& BoxExtent) const;
    bool SetActorLocation(const FVector& NewLocation, bool bSweep = false, bool bTeleport = false);
    FRotator GetActorRotation() const;
    bool TeleportTo(const FVector& DestLocation, const FRotator& DestRotation);
    void SetActorScale3D(const FVector& NewScale3D);
    void SetActorEnableCollision(bool bNewActorEnableCollision);
    void K2_DestroyActor();
    TArray<UActorComponent*> GetComponentsByClass(UClass* ComponentClass) const;
    FVector GetVelocity() const;
    float GetNetPriority(const FVector& ViewPos, const FVector& ViewDir, AActor* Viewer, AActor* ViewTarget, UActorChannel* InChannel, float Time, bool bLowBandwidth);
    bool GetNetDormancy(const FVector& ViewPos, const FVector& ViewDir, AActor* Viewer, AActor* ViewTarget, UActorChannel* InChannel, float Time, bool bLowBandwidth);
    bool IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const;
    bool IsRelevancyOwnerFor(const AActor* ReplicatedActor, const AActor* ActorOwner, const AActor* ConnectionActor) const;
    const AActor* GetNetOwner() const;
    void ForceNetUpdate();
    void CallPreReplication(UNetDriver* NetDriver);

    void FlushNetDormancy();
    void SetNetDormancy(ENetDormancy NewDormancy);

    void ReceiveTick(float DeltaSeconds);

    static void Hook();

private:
    ENetMode InternalGetNetMode() const;

    static void TickReceiveTickHK(AActor* This, float DeltaSeconds);
};

inline ENetMode AActor::GetNetMode() const
{
    if (IsRunningDedicatedServer() && (NetDriverName == NAME_None || NetDriverName == NAME_GameNetDriver))
    {
        return NM_DedicatedServer;
    }

    return InternalGetNetMode();
}

inline bool AActor::IsNetMode(ENetMode Mode) const
{
    if (Mode == NM_DedicatedServer)
    {
        return IsRunningDedicatedServer();
    }

    if (NetDriverName == NAME_None || NetDriverName == NAME_GameNetDriver)
    {
        return !IsRunningDedicatedServer() && InternalGetNetMode() == Mode;
    }

    return InternalGetNetMode() == Mode;
}
