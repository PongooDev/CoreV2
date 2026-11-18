#include "pch.h"
#include "Creative/FortVolume.h"
#include "AsyncTaskQueue.h"
#include "Kismet/GameplayStatics.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

#include <limits>

UAsyncTaskQueue* AFortVolume::GetTaskQueue()
{
    return Natives::AFortVolume_GetTaskQueue ? static_cast<UAsyncTaskQueue*>(Natives::AFortVolume_GetTaskQueue(this)) : nullptr;
}

FMulticastScriptDelegate* AFortVolume::ClearVolumeOnServer()
{
    return Natives::AFortVolume_ClearVolumeOnServer ? static_cast<FMulticastScriptDelegate*>(Natives::AFortVolume_ClearVolumeOnServer(this)) : nullptr;
}

int32_t AFortVolume::TaskQueueOffset()
{
    return Member_TaskQueue.IsAvailable() ? Member_TaskQueue.GetOffsetChecked() : -1;
}

int32_t AFortVolume::OnFinishVolumeClearOffset()
{
    return Member_OnFinishVolumeClear.IsAvailable() ? Member_OnFinishVolumeClear.GetOffsetChecked() : -1;
}

bool AFortVolume::ContainsLocation(const FVector& Location) const
{
    FVector Origin;
    FVector Extent;
    GetActorBounds(false, Origin, Extent);
    return Extent.X > 0.0f && Extent.Y > 0.0f && Location.X >= Origin.X - Extent.X && Location.X <= Origin.X + Extent.X && Location.Y >= Origin.Y - Extent.Y
        && Location.Y <= Origin.Y + Extent.Y;
}

AFortPlayerStartCreative* AFortVolume::FindPlayerStart(const wchar_t* Tag, const FVector& Near) const
{
    AFortPlayerStartCreative* Best = nullptr;
    float BestDistance = std::numeric_limits<float>::max();
    UClass* StartClass = AFortPlayerStartCreative::StaticClass();
    for (AActor* Actor : StartClass ? UGameplayStatics::GetAllActorsOfClass(this, StartClass) : TArray<AActor*>())
    {
        AFortPlayerStartCreative* Start = static_cast<AFortPlayerStartCreative*>(Actor);
        const FVector Location = Start ? Start->GetTransform().GetLocation() : FVector();
        if (!Start || !ContainsLocation(Location) || (Tag && !Start->HasTag(Tag)))
        {
            continue;
        }
        const float Distance = (Location - Near).SizeSquared();
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            Best = Start;
        }
    }
    return Best;
}

AFortPlayerStartCreative* AFortVolume::FindIslandStart() const
{
    // The game's pick (7.20 0xE8BFF0): the world's AFortPlayerStartCreative in actor order whose CreativeLinkComponent's
    // LinkedVolume is the island and which is set as an island start. The game also skips a start it can't claim (full,
    // or its spot blocked for the player's pawn: AFortPlayerStartCreative::Claim, 7.20 0x1084E40, and its check
    // 0x1084900) and takes an unclaimed one before one claimed this frame; that occupancy isn't reflected, so it isn't
    // read here, and the pawn's own TeleportTo finds it a free spot by the start instead. A start whose link has no
    // volume yet is taken by the volume's box (ContainsLocation).
    UClass* StartClass = AFortPlayerStartCreative::StaticClass();
    if (!StartClass || !AFortPlayerStartCreative::HasUseAsIslandStart())
    {
        return nullptr;
    }
    const bool bLinked = AFortPlayerStartCreative::HasCreativeLinkComponent() && UFortCreativeVolumeLinkComponent::HasLinkedVolume();
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(this, StartClass))
    {
        AFortPlayerStartCreative* Start = static_cast<AFortPlayerStartCreative*>(Actor);
        if (!Start || !Start->bUseAsIslandStart)
        {
            continue;
        }
        UFortCreativeVolumeLinkComponent* Link = bLinked ? static_cast<UFortCreativeVolumeLinkComponent*>(Start->CreativeLinkComponent) : nullptr;
        AFortVolume* Linked = Link ? static_cast<AFortVolume*>(Link->LinkedVolume) : nullptr;
        if (Linked ? Linked == this : ContainsLocation(Start->GetTransform().GetLocation()))
        {
            return Start;
        }
    }
    return nullptr;
}

AFortVolume* AFortVolumeManager::GetVolumeForActor(const AActor* Actor)
{
    Reflection::FParams Params(FindStaticFunction_GetVolumeForActor(), true);
    if (!Actor || !Params.Set(L"Actor", const_cast<AActor*>(Actor)) || !Params.Call(this))
    {
        return nullptr;
    }
    AFortVolume** Result = Params.Get<AFortVolume*>(L"ReturnValue");
    return Result ? *Result : nullptr;
}

void AFortVolumeManager::ResetVolumeAndLevelRecord(AFortVolume* Volume, AFortPlayerController* RequestingFortPC)
{
    Reflection::FParams Params(FindStaticFunction_ResetVolumeAndLevelRecord(), true);
    if (!Volume || !Params.Set(L"Volume", Volume))
    {
        return;
    }
    // 7.10 (CL-4667333) also takes the player who asked for it; a reset of the server's own (an unload, a reload) has none.
    if (RequestingFortPC && Params.Has(L"RequestingFortPC"))
    {
        Params.Set(L"RequestingFortPC", RequestingFortPC);
    }
    Params.Call(this);
}

AFortVolumeManager* AFortVolumeManager::Get(const UObject* WorldContextObject)
{
    // One per world (AFortGameStateAthena::VolumeManager on 7.00): the first the world has.
    UClass* Class = StaticClass();
    const TArray<AActor*> Managers = Class && WorldContextObject ? UGameplayStatics::GetAllActorsOfClass(WorldContextObject, Class) : TArray<AActor*>();
    return Managers.Num() > 0 ? static_cast<AFortVolumeManager*>(Managers[0]) : nullptr;
}

bool AFortPlayerStartCreative::HasTag(const wchar_t* Tag) const
{
    const FGameplayTagContainer& Tags = PlayerStartTags;
    const FName TagName(Tag, FNAME_Find);
    if (TagName.IsNone())
    {
        return false;
    }
    for (const FGameplayTag& Each : Tags.GameplayTags)
    {
        if (Each.TagName == TagName)
        {
            return true;
        }
    }
    return false;
}
