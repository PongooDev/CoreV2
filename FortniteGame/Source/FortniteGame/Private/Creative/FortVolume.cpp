#include "pch.h"
#include "Creative/FortVolume.h"
#include "Kismet/GameplayStatics.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

#include <limits>

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
