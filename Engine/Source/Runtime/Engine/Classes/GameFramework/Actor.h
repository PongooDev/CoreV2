#pragma once

#include "Containers/EnumAsByte.h"
#include "Math/Vector.h"
#include "ObjectLayout.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"

class ULevel;
class UNetDriver;
class UActorChannel;
class UWorld;

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
    DefineUProperty(FName, NetDriverName)
    DefineUProperty(TEnumAsByte<ENetRole>, Role)
    DefineUProperty(TEnumAsByte<ENetRole>, RemoteRole)
    DefineUProperty(AActor*, Owner)
    DefineUProperty(int32_t, NetTag)
    DefineUProperty(float, NetUpdateFrequency)
    DefineUProperty(float, MinNetUpdateFrequency)
    DefineNativeMember(TEnumAsByte<ENetDormancy>, NetDormancy, AActor_NetDormancy)
    DefineNativeBitfield(bActorInitialized, AActor_bActorInitialized, Natives::AActor_bActorInitializedMask)

    ENetRole GetRemoteRole() const { return RemoteRole; }
    AActor* GetOwner() const { return Owner; }
    bool IsNetStartupActor() const { return bNetStartup; }
    bool IsActorInitialized() const { return bActorInitialized; }
    const FName& GetNetDriverName() const { return NetDriverName; }

    ULevel* GetLevel() const;
    UWorld* GetWorld() const;
    UNetDriver* GetNetDriver() const;

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

    static void Hook();

private:
    static inline void (*FlushNetDormancyOG)(AActor* This) = nullptr;
    static inline void (*SetNetDormancyOG)(AActor* This, ENetDormancy NewDormancy) = nullptr;
    static void FlushNetDormancyHK(AActor* This);
    static void SetNetDormancyHK(AActor* This, ENetDormancy NewDormancy);
};
