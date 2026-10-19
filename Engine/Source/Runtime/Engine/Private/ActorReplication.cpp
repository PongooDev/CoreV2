#include "pch.h"
#include "GameFramework/Actor.h"
#include "Natives.h"

float AActor::GetNetPriority(const FVector& ViewPos, const FVector& ViewDir, AActor* Viewer, AActor* ViewTarget, UActorChannel* InChannel, float Time, bool bLowBandwidth)
{
    return CallVirtual<float>(this, Natives::AActor_GetNetPriorityIndex, &ViewPos, &ViewDir, Viewer, ViewTarget, InChannel, Time, bLowBandwidth);
}

bool AActor::GetNetDormancy(const FVector& ViewPos, const FVector& ViewDir, AActor* Viewer, AActor* ViewTarget, UActorChannel* InChannel, float Time, bool bLowBandwidth)
{
    return CallVirtual<bool>(this, Natives::AActor_GetNetDormancyIndex, &ViewPos, &ViewDir, Viewer, ViewTarget, InChannel, Time, bLowBandwidth);
}

bool AActor::IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const
{
    return CallVirtual<bool>(this, Natives::AActor_IsNetRelevantForIndex, RealViewer, ViewTarget, &SrcLocation);
}

const AActor* AActor::GetNetOwner() const
{
    return CallVirtual<const AActor*>(this, Natives::AActor_GetNetOwnerIndex);
}
