#include "pch.h"
#include "Creative/FortVolume.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Natives.h"
#include "Templates/TypeHash.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    // An engine TMap<FCreativeActorId, V> (a TSet of 32-byte elements: the key's TArray<FName>, the value, HashNextId at
    // +0x18): its sparse array's data and count at +0/+8, free count at +0x34, hash (inline at +0x38, else the
    // allocation at +0x40) and its size at +0x48.
    const uint8_t* FindActorIdElement(const uint8_t* Set, const TArray<FName>& ActorId)
    {
        if (!Set)
        {
            return nullptr;
        }
        const int32_t Num = *reinterpret_cast<const int32_t*>(Set + 0x8);
        const int32_t NumFree = *reinterpret_cast<const int32_t*>(Set + 0x34);
        const int32_t HashSize = *reinterpret_cast<const int32_t*>(Set + 0x48);
        if (Num == NumFree || HashSize <= 0)
        {
            return nullptr;
        }
        // GetTypeHash(FCreativeActorId) (8.51 0x1145390): each name's hash combined in order.
        uint32_t Hash = 0;
        for (const FName& Name : ActorId)
        {
            Hash = HashCombine(Hash, GetTypeHash(Name));
        }
        const int32_t* HashAllocation = *reinterpret_cast<const int32_t* const*>(Set + 0x40);
        const int32_t* Buckets = HashAllocation ? HashAllocation : reinterpret_cast<const int32_t*>(Set + 0x38);
        const uint8_t* Elements = *reinterpret_cast<const uint8_t* const*>(Set);
        for (int32_t Index = Buckets[Hash & static_cast<uint32_t>(HashSize - 1)]; Index != -1;)
        {
            const uint8_t* Element = Elements + 32 * static_cast<size_t>(Index);
            const TArray<FName>& Key = *reinterpret_cast<const TArray<FName>*>(Element);
            bool bEqual = Key.Num() == ActorId.Num();
            for (int32_t Name = 0; bEqual && Name < Key.Num(); ++Name)
            {
                bEqual = Key[Name] == ActorId[Name];
            }
            if (bEqual)
            {
                return Element;
            }
            Index = *reinterpret_cast<const int32_t*>(Element + 0x18);
        }
        return nullptr;
    }

    int32_t ConsoleInt(const wchar_t* Name, int32_t Default)
    {
        IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable ? Variable->GetInt() : Default;
    }

    float ConsoleFloat(const wchar_t* Name, float Default)
    {
        IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable ? Variable->GetFloat() : Default;
    }

    bool IsChildOf(const AActor* Actor, const UClass* Class)
    {
        return Actor && Class && Actor->GetClass()->IsChildOf(Class);
    }

    struct FActorIdCount
    {
        TArray<FName> ActorId;
        int32_t Count;
    };

    FActorIdCount* FindCount(TArray<FActorIdCount>& Counts, const TArray<FName>& ActorId)
    {
        for (FActorIdCount& Each : Counts)
        {
            bool bEqual = Each.ActorId.Num() == ActorId.Num();
            for (int32_t Name = 0; bEqual && Name < ActorId.Num(); ++Name)
            {
                bEqual = Each.ActorId[Name] == ActorId[Name];
            }
            if (bEqual)
            {
                return &Each;
            }
        }
        return nullptr;
    }
}

TArray<FName> FFortCreativeBudgetTracker::MakeActorId(const AActor* Actor)
{
    TArray<FName> ActorId;
    ActorId.Add(Actor->GetClass()->GetFName());
    if (IsChildOf(Actor, AStaticMeshActor::StaticClass()))
    {
        UStaticMeshComponent* Component = static_cast<const AStaticMeshActor*>(Actor)->StaticMeshComponent;
        UStaticMesh* Mesh = Component ? static_cast<UStaticMesh*>(Component->StaticMesh) : nullptr;
        if (Mesh)
        {
            ActorId.Add(Mesh->GetFName());
        }
    }
    return ActorId;
}

int32_t FFortCreativeBudgetTracker::CostUsedByAsset(const TArray<FName>& ActorId)
{
    const uint8_t* Cost = FindActorIdElement(static_cast<const uint8_t*>(Natives::FortCreativeAssetCosts), ActorId);
    return Cost ? static_cast<int32_t>(static_cast<float>(*reinterpret_cast<const int32_t*>(Cost + 0x10)) * ConsoleFloat(L"Creative.AssetCostMultiplier", 1.0f)) : 0;
}

int32_t FFortCreativeBudgetTracker::InstanceCostOf(const TArray<FName>& ActorId)
{
    const uint8_t* Cost = FindActorIdElement(static_cast<const uint8_t*>(Natives::FortCreativeAssetCosts), ActorId);
    return Cost ? static_cast<int32_t>(static_cast<float>(*reinterpret_cast<const int32_t*>(Cost + 0x14)) * ConsoleFloat(L"Creative.InstanceCostMultiplier", 1.0f))
                : ConsoleInt(L"Creative.FixedInstanceCost", 40000);
}

const int32_t* FFortCreativeBudgetTracker::FindAssetInstances(const TArray<FName>& ActorId) const
{
    const uint8_t* Map = reinterpret_cast<const uint8_t*>(this) + Member_GlobalBudget.GetOffsetChecked() + sizeof(FMemoryBudget);
    const uint8_t* Element = FindActorIdElement(Map, ActorId);
    return Element ? reinterpret_cast<const int32_t*>(Element + 0x10) : nullptr;
}

bool FFortCreativeClassInstanceTracker::CanAddActors(const TArray<AActor*>& NewActors, const TArray<AActor*>& ActorsToRemove) const
{
    const TArray<UClass*>& Limited = ParentClassesAtLimit;
    const auto LimitedClassOf = [&Limited](const AActor* Actor) -> UClass*
    {
        for (UClass* Class : Limited)
        {
            if (IsChildOf(Actor, Class))
            {
                return Class;
            }
        }
        return nullptr;
    };
    TArray<UClass*> Classes;
    TArray<int32_t> Counts;
    for (AActor* Actor : NewActors)
    {
        if (UClass* Class = Actor ? LimitedClassOf(Actor) : nullptr)
        {
            int32_t Index = 0;
            while (Index < Classes.Num() && Classes[Index] != Class)
            {
                ++Index;
            }
            if (Index == Classes.Num())
            {
                Classes.Add(Class);
                Counts.Add(0);
            }
            ++Counts[Index];
        }
    }
    for (AActor* Actor : ActorsToRemove)
    {
        UClass* Class = Actor ? LimitedClassOf(Actor) : nullptr;
        for (int32_t Index = 0; Class && Index < Classes.Num(); ++Index)
        {
            if (Classes[Index] == Class)
            {
                if (Counts[Index] <= 1)
                {
                    Classes.RemoveAt(Index);
                    Counts.RemoveAt(Index);
                }
                else
                {
                    --Counts[Index];
                }
                break;
            }
        }
    }
    return Classes.Num() > 0;
}

bool FFortCreativeBudgetTracker::CanAddActor(AActor* NewActor, const TArray<AActor*>& ActorsToRemove) const
{
    if (ConsoleInt(L"Creative.Budget.Enabled", 1) != 1)
    {
        return false;
    }
    TArray<AActor*> NewActors;
    NewActors.Add(NewActor);
    return CanAddActors(NewActors, ActorsToRemove);
}

bool FFortCreativeBudgetTracker::CanAddActors(const TArray<AActor*>& NewActors, const TArray<AActor*>& ActorsToRemove) const
{
    if (ConsoleInt(L"Creative.Budget.Enabled", 1) != 1)
    {
        return false;
    }
    if (ActorsToRemove.Num() == 0)
    {
        using FCanAddActors = bool (*)(const FFortCreativeBudgetTracker*, const TArray<AActor*>*);
        return Natives::FFortCreativeBudgetTracker_CanAddActors && reinterpret_cast<FCanAddActors>(Natives::FFortCreativeBudgetTracker_CanAddActors)(this, &NewActors);
    }
    // A replacement is refused unless it adds an actor of a class at its limit (the build's own rule, 8.20 to 8.51: why
    // walls and floors can't be copied onto themselves).
    if (!ClassTracker.CanAddActors(NewActors, ActorsToRemove))
    {
        return false;
    }
    // 9.20 (CL-6822798) drops FCreativeActorId: the cost table (0x57DFDB0) and the instance counts are keyed by the
    // actor's TSoftClassPtr (56-byte elements, the actor cost 0x141E6B0), which the 8.51 reading below can't walk. The
    // build has no replacement check of its own, so the new actors go through its CanAddActors (0x140C160) alone.
    static const bool bActorIdKeys = FindObject<UScriptStruct>(nullptr, L"/Script/FortniteGame.CreativeActorId") != nullptr;
    if (!bActorIdKeys)
    {
        using FCanAddActors = bool (*)(const FFortCreativeBudgetTracker*, const TArray<AActor*>*);
        return Natives::FFortCreativeBudgetTracker_CanAddActors && reinterpret_cast<FCanAddActors>(Natives::FFortCreativeBudgetTracker_CanAddActors)(this, &NewActors);
    }

    TArray<FActorIdCount> NewCounts;
    int32_t NewInstanceCost = 0;
    for (AActor* Actor : NewActors)
    {
        if (!Actor)
        {
            continue;
        }
        TArray<FName> ActorId = MakeActorId(Actor);
        if (FActorIdCount* Count = FindCount(NewCounts, ActorId))
        {
            ++Count->Count;
        }
        else
        {
            NewCounts.Add(FActorIdCount{ ActorId, 1 });
        }
        NewInstanceCost += InstanceCostOf(ActorId);
    }

    int32_t RemovedInstanceCost = 0;
    TArray<TArray<FName>> RemovedIds;
    for (AActor* Actor : ActorsToRemove)
    {
        if (!Actor)
        {
            continue;
        }
        TArray<FName> ActorId = MakeActorId(Actor);
        FActorIdCount* Count = FindCount(NewCounts, ActorId);
        if (Count && Count->Count > 0)
        {
            --Count->Count;
        }
        else
        {
            if (Count)
            {
                NewCounts.RemoveAt(static_cast<int32_t>(Count - NewCounts.GetData()));
            }
            bool bListed = false;
            for (const TArray<FName>& Removed : RemovedIds)
            {
                bool bEqual = Removed.Num() == ActorId.Num();
                for (int32_t Name = 0; bEqual && Name < ActorId.Num(); ++Name)
                {
                    bEqual = Removed[Name] == ActorId[Name];
                }
                bListed = bListed || bEqual;
            }
            if (!bListed)
            {
                RemovedIds.Add(ActorId);
            }
        }
        RemovedInstanceCost += InstanceCostOf(ActorId);
    }

    int32_t NewAssetCost = 0;
    for (const FActorIdCount& Count : NewCounts)
    {
        const int32_t* Tracked = FindAssetInstances(Count.ActorId);
        if (Tracked && *Tracked <= Count.Count)
        {
            NewAssetCost += CostUsedByAsset(Count.ActorId);
        }
    }
    int32_t RemovedAssetCost = 0;
    for (const TArray<FName>& Removed : RemovedIds)
    {
        RemovedAssetCost += CostUsedByAsset(Removed);
    }
    const FMemoryBudget& Budget = GlobalBudget;
    const int32_t Free = Budget.TotalMemory - Budget.InUseMemory >= 0 ? Budget.TotalMemory - Budget.InUseMemory : 0;
    return RemovedAssetCost + RemovedInstanceCost < NewInstanceCost + NewAssetCost + Free;
}
