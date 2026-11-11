#pragma once

#include "Delegates/DelegateBase.h"
#include "HAL/UnrealMemory.h"

#include <cstddef>
#include <cstring>

// Epic's FMulticastDelegateBase<FWeakObjectPtr>: a multicast delegate's invocation list of FDelegateBases, its compaction
// threshold and lock count, in the build's layout (see FDelegateBase). Up to 4.1 the list is a TArray<FDelegateBase,
// TInlineAllocator<1>>; 4.2's is a TArray<FDelegateBase> (UConsole::RegisterConsoleAutoCompleteEntries at 0x4917F50: Data,
// ArrayNum +8, CompactionThreshold +0x10, lock count +0x14). Core reaches the engine's delegates through this (a
// TMulticastDelegate member found by reflection or a finder), and can own one too.
class FMulticastDelegateBase
{
public:
    // Epic's constructor: an empty list, compaction threshold 2.
    FMulticastDelegateBase()
    {
        CompactionThreshold() = 2;
    }

    ~FMulticastDelegateBase()
    {
        Clear();
        ResizeAllocation(0);
    }

    FMulticastDelegateBase(const FMulticastDelegateBase&) = delete;
    FMulticastDelegateBase& operator=(const FMulticastDelegateBase&) = delete;

    // Unbinds every instance, then compacts the list.
    void Clear()
    {
        for (int32_t Index = 0; Index < ArrayNum(); ++Index)
        {
            Invocation(Index).Unbind();
        }
        CompactInvocationList(false);
    }

    // Whether the memory at Address is a multicast delegate as Epic's constructor leaves it (no allocation, no entries,
    // compaction threshold 2, not locked): what an unreflected delegate member of a class default object is. Core checks
    // an offset a finder read off a broadcast with it before it binds anything there.
    static bool IsDefaultConstructedAt(const void* Address)
    {
        if (!Address)
        {
            return false;
        }
        const FMulticastDelegateBase& Delegate = *static_cast<const FMulticastDelegateBase*>(Address);
        const bool bNoAllocation = DelegateBase::UsesHeapAllocator() ? !Delegate.Heap().Data : !Delegate.Inline().InvocationList.Allocator.SecondaryData;
        // The inline list's capacity is 0 or its one inline entry, as the build's TArray starts it.
        const int32_t Max = Delegate.ArrayMax();
        return bNoAllocation && Delegate.ArrayNum() == 0 && (Max == 0 || (Max == 1 && !DelegateBase::UsesHeapAllocator())) && Delegate.CompactionThreshold() == 2
            && Delegate.InvocationListLockCount() == 0;
    }

    bool IsBound() const
    {
        for (int32_t Index = 0; Index < ArrayNum(); ++Index)
        {
            if (Invocation(Index).GetDelegateInstanceProtected())
            {
                return true;
            }
        }
        return false;
    }

    bool IsBoundToObject(void const* InUserObject) const
    {
        for (int32_t Index = 0; Index < ArrayNum(); ++Index)
        {
            IDelegateInstance* DelegateInstance = Invocation(Index).GetDelegateInstanceProtected();
            if (DelegateInstance && DelegateInstance::HasSameObject(DelegateInstance, InUserObject))
            {
                return true;
            }
        }
        return false;
    }

    // Removes every instance bound to InUserObject (an AddUObject's object). While a broadcast holds the list, they are
    // unbound and the next Add compacts them out.
    void RemoveAll(const void* InUserObject)
    {
        if (InvocationListLockCount() > 0)
        {
            bool NeedsCompacted = false;
            for (int32_t Index = 0; Index < ArrayNum(); ++Index)
            {
                FDelegateBase& DelegateBaseRef = Invocation(Index);
                IDelegateInstance* DelegateInstance = DelegateBaseRef.GetDelegateInstanceProtected();
                if (DelegateInstance && DelegateInstance::HasSameObject(DelegateInstance, InUserObject))
                {
                    DelegateBaseRef.Unbind();
                    NeedsCompacted = true;
                }
            }
            if (NeedsCompacted)
            {
                CompactionThreshold() = 0;
            }
            return;
        }

        for (int32_t Index = 0; Index < ArrayNum();)
        {
            IDelegateInstance* DelegateInstance = Invocation(Index).GetDelegateInstanceProtected();
            if (!DelegateInstance || DelegateInstance::HasSameObject(DelegateInstance, InUserObject) || DelegateInstance::IsCompactable(DelegateInstance))
            {
                RemoveAtSwap(Index);
            }
            else
            {
                ++Index;
            }
        }
        CompactionThreshold() = ArrayNum() * 2 > 2 ? ArrayNum() * 2 : 2;
        Shrink();
    }

protected:
    // The bound delegate's instance moves into the list (FDelegateBase's move: its bytes), after a compaction that obeys
    // and decays the threshold, so even a delegate added to rarely gets compacted. Returns the instance's handle.
    FDelegateHandle AddInternal(FDelegateBase&& NewDelegateBaseRef)
    {
        CompactInvocationList(true);
        const FDelegateHandle Result = NewDelegateBaseRef.GetHandle();
        const int32_t Index = ArrayNum();
        if (Index + 1 > ArrayMax())
        {
            ResizeGrow();
        }
        ++ArrayNum();
        NewDelegateBaseRef.RelocateTo(&Invocation(Index));
        return Result;
    }

    bool RemoveDelegateInstance(FDelegateHandle Handle)
    {
        for (int32_t Index = 0; Index < ArrayNum(); ++Index)
        {
            FDelegateBase& DelegateBaseRef = Invocation(Index);
            IDelegateInstance* DelegateInstance = DelegateBaseRef.GetDelegateInstanceProtected();
            if (DelegateInstance && DelegateInstance::GetHandle(DelegateInstance) == Handle)
            {
                DelegateBaseRef.Unbind();
                CompactInvocationList();
                // Each binding has its own handle.
                return true;
            }
        }
        return false;
    }

    // Removes the empty and compactable entries (an instance whose object is gone), unless a broadcast holds the list.
    // With CheckThreshold (an Add), only once the threshold, decremented each time, is no longer above the count.
    void CompactInvocationList(bool CheckThreshold = false)
    {
        if (InvocationListLockCount() > 0)
        {
            return;
        }
        if (CheckThreshold && --CompactionThreshold() > ArrayNum())
        {
            return;
        }

        const int32_t OldNumItems = ArrayNum();
        for (int32_t Index = 0; Index < ArrayNum();)
        {
            IDelegateInstance* DelegateInstance = Invocation(Index).GetDelegateInstanceProtected();
            if (!DelegateInstance || DelegateInstance::IsCompactable(DelegateInstance))
            {
                RemoveAtSwap(Index);
            }
            else
            {
                ++Index;
            }
        }

        CompactionThreshold() = ArrayNum() * 2 > 2 ? ArrayNum() * 2 : 2;
        if (OldNumItems > CompactionThreshold())
        {
            Shrink();
        }
    }

    void LockInvocationList() const
    {
        ++InvocationListLockCount();
    }

    void UnlockInvocationList() const
    {
        --InvocationListLockCount();
    }

    int32_t GetInvocationListNum() const
    {
        return ArrayNum();
    }

    // The build's elements are FDelegateBase::BuildSize() apart.
    FDelegateBase& Invocation(int32_t Index) const
    {
        return *reinterpret_cast<FDelegateBase*>(GetData() + Index * FDelegateBase::BuildSize());
    }

private:
    struct alignas(16) FInlineInvocationList
    {
        struct alignas(16) FInvocationList
        {
            struct alignas(16) FAllocator
            {
                alignas(16) uint8_t InlineData[sizeof(FDelegateBase)];
                uint8_t* SecondaryData;
            };

            FAllocator Allocator;
            int32_t ArrayNum;
            int32_t ArrayMax;
        };

        FInvocationList InvocationList;
        int32_t CompactionThreshold;
        int32_t InvocationListLockCount;
    };

    struct FHeapInvocationList
    {
        uint8_t* Data;
        int32_t ArrayNum;
        int32_t ArrayMax;
        int32_t CompactionThreshold;
        int32_t InvocationListLockCount;
    };

    FInlineInvocationList& Inline() const { return *reinterpret_cast<FInlineInvocationList*>(const_cast<uint8_t*>(Storage)); }
    FHeapInvocationList& Heap() const { return *reinterpret_cast<FHeapInvocationList*>(const_cast<uint8_t*>(Storage)); }

    int32_t& ArrayNum() const { return DelegateBase::UsesHeapAllocator() ? Heap().ArrayNum : Inline().InvocationList.ArrayNum; }
    int32_t& ArrayMax() const { return DelegateBase::UsesHeapAllocator() ? Heap().ArrayMax : Inline().InvocationList.ArrayMax; }
    int32_t& CompactionThreshold() const { return DelegateBase::UsesHeapAllocator() ? Heap().CompactionThreshold : Inline().CompactionThreshold; }
    int32_t& InvocationListLockCount() const { return DelegateBase::UsesHeapAllocator() ? Heap().InvocationListLockCount : Inline().InvocationListLockCount; }

    uint8_t* GetData() const
    {
        if (DelegateBase::UsesHeapAllocator())
        {
            return Heap().Data;
        }
        FInlineInvocationList::FInvocationList::FAllocator& Allocator = Inline().InvocationList.Allocator;
        return Allocator.SecondaryData ? Allocator.SecondaryData : Allocator.InlineData;
    }

    // TArray::RemoveAtSwap: the entry is destroyed (its instance unbound) and the last one moves into its place.
    void RemoveAtSwap(int32_t Index)
    {
        Invocation(Index).~FDelegateBase();
        const int32_t Last = ArrayNum() - 1;
        if (Index != Last)
        {
            memcpy(static_cast<void*>(&Invocation(Index)), &Invocation(Last), FDelegateBase::BuildSize());
        }
        --ArrayNum();
    }

    // TArray::ResizeGrow for one more entry, with the allocator's CalculateSlackGrow: TInlineAllocator<1> keeps a single
    // entry inline; the heap's (DefaultCalculateSlackGrow) first allocation is 4 entries, after that n + 3n/8 + 16.
    void ResizeGrow()
    {
        const int32_t NumElements = ArrayNum() + 1;
        int32_t NewMax = 1;
        if (DelegateBase::UsesHeapAllocator() || NumElements > 1)
        {
            NewMax = ArrayMax() || NumElements > 4 ? NumElements + 3 * NumElements / 8 + 16 : 4;
        }
        ArrayMax() = NewMax;
        ResizeAllocation(NewMax);
    }

    // TArray::Shrink: the allocation down to the entries there are.
    void Shrink()
    {
        const int32_t NewMax = DelegateBase::UsesHeapAllocator() ? ArrayNum() : ArrayNum() <= 1 ? 1 : ArrayNum();
        if (NewMax != ArrayMax())
        {
            ArrayMax() = NewMax;
            ResizeAllocation(NewMax);
        }
    }

    // The allocator's ResizeAllocation for ArrayNum() live entries: FHeapAllocator reallocates (to 0 frees);
    // TInlineAllocator<1> keeps one entry inline and moves them between that and its secondary allocation.
    void ResizeAllocation(int32_t NumElements)
    {
        const size_t ElementSize = FDelegateBase::BuildSize();
        if (DelegateBase::UsesHeapAllocator())
        {
            FHeapInvocationList& List = Heap();
            if (List.Data || NumElements)
            {
                List.Data = static_cast<uint8_t*>(FMemory::Realloc(List.Data, NumElements * ElementSize));
            }
            return;
        }

        // ArrayNum() is the entries there now: at most one when they are inline.
        FInlineInvocationList::FInvocationList::FAllocator& Allocator = Inline().InvocationList.Allocator;
        if (NumElements <= 1)
        {
            if (Allocator.SecondaryData)
            {
                if (ArrayNum() > 0)
                {
                    memcpy(Allocator.InlineData, Allocator.SecondaryData, ElementSize);
                }
                FMemory::Free(Allocator.SecondaryData);
                Allocator.SecondaryData = nullptr;
            }
        }
        else if (!Allocator.SecondaryData)
        {
            Allocator.SecondaryData = static_cast<uint8_t*>(FMemory::Realloc(nullptr, NumElements * ElementSize, alignof(FDelegateBase)));
            if (ArrayNum() > 0)
            {
                memcpy(Allocator.SecondaryData, Allocator.InlineData, ElementSize);
            }
        }
        else
        {
            Allocator.SecondaryData = static_cast<uint8_t*>(FMemory::Realloc(Allocator.SecondaryData, NumElements * ElementSize, alignof(FDelegateBase)));
        }
    }

    alignas(16) uint8_t Storage[sizeof(FInlineInvocationList)] = {};

    friend struct FMulticastDelegateLayout;
};

struct FMulticastDelegateLayout
{
    using FInline = FMulticastDelegateBase::FInlineInvocationList;
    using FHeap = FMulticastDelegateBase::FHeapInvocationList;
    static_assert(offsetof(FInline, InvocationList.ArrayNum) == 0x50);
    static_assert(offsetof(FInline, CompactionThreshold) == 0x60);
    static_assert(offsetof(FInline, InvocationListLockCount) == 0x64);
    static_assert(sizeof(FInline) == 0x70);
    static_assert(offsetof(FHeap, ArrayNum) == 0x08);
    static_assert(offsetof(FHeap, CompactionThreshold) == 0x10);
    static_assert(offsetof(FHeap, InvocationListLockCount) == 0x14);
};
