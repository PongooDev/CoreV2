#pragma once

#include "Delegates/IDelegateInstance.h"
#include "HAL/UnrealMemory.h"

#include <cstddef>

struct alignas(16) FAlignedInlineDelegateType
{
    uint8_t Data[16];
};

namespace DelegateBase
{
    // NUM_DELEGATE_INLINE_BYTES 0 (4.2): the delegate allocator is FHeapAllocator and the invocation list a plain TArray.
    inline bool UsesHeapAllocator()
    {
        return Natives::NumDelegateInlineBytes == 0;
    }
}

// The storage holds either layout: up to 4.1 TInlineAllocator<2> (InlineData[2], SecondaryData) with DelegateSize at
// 0x30, sizeof 0x40; on 4.2 FHeapAllocator (Data) with DelegateSize at 8, sizeof 0x10 (4.2's
// IConsoleManager::ForEachConsoleObjectThatStartsWith, 0x12917B0: cmp [Visitor+8], 0; mov rcx, [Visitor]). A Core-made
// delegate keeps 0x40 bytes either way, and the engine reads only the build's first bytes of it.
class FDelegateBase
{
public:
    FDelegateBase() = default;

    ~FDelegateBase()
    {
        Unbind();
    }

    FDelegateBase(const FDelegateBase&) = delete;
    FDelegateBase& operator=(const FDelegateBase&) = delete;

    // sizeof(FDelegateBase) in the build: the stride of an engine invocation list.
    static size_t BuildSize()
    {
        return DelegateBase::UsesHeapAllocator() ? sizeof(FHeapDelegateBase) : sizeof(FInlineDelegateBase);
    }

    bool IsBound() const
    {
        IDelegateInstance* Ptr = GetDelegateInstanceProtected();
        return Ptr && DelegateInstance::IsSafeToExecute(Ptr);
    }

    void Unbind()
    {
        if (IDelegateInstance* Ptr = GetDelegateInstanceProtected())
        {
            DelegateInstance::Destroy(Ptr);
            ResizeAllocation(0);
            DelegateSize() = 0;
        }
    }

    FDelegateHandle GetHandle() const
    {
        IDelegateInstance* Ptr = GetDelegateInstanceProtected();
        return Ptr ? DelegateInstance::GetHandle(Ptr) : FDelegateHandle();
    }

    IDelegateInstance* GetDelegateInstanceProtected() const
    {
        return DelegateSize() ? static_cast<IDelegateInstance*>(GetAllocation()) : nullptr;
    }

    void* Allocate(int32_t Size)
    {
        if (IDelegateInstance* CurrentInstance = GetDelegateInstanceProtected())
        {
            DelegateInstance::Destroy(CurrentInstance);
        }

        const int32_t NewDelegateSize = (Size + static_cast<int32_t>(sizeof(FAlignedInlineDelegateType)) - 1) / static_cast<int32_t>(sizeof(FAlignedInlineDelegateType));
        if (DelegateSize() != NewDelegateSize)
        {
            ResizeAllocation(NewDelegateSize);
            DelegateSize() = NewDelegateSize;
        }
        return GetAllocation();
    }

private:
    static constexpr int32_t NumInlineElements = 2;

    struct alignas(16) FInlineDelegateBase
    {
        struct alignas(16) FDelegateAllocator
        {
            FAlignedInlineDelegateType InlineData[NumInlineElements];
            void* SecondaryData;
        };

        FDelegateAllocator DelegateAllocator;
        int32_t DelegateSize;
    };

    struct FHeapDelegateBase
    {
        void* Data;
        int32_t DelegateSize;
    };

    FInlineDelegateBase& Inline() { return *reinterpret_cast<FInlineDelegateBase*>(Storage); }
    const FInlineDelegateBase& Inline() const { return *reinterpret_cast<const FInlineDelegateBase*>(Storage); }
    FHeapDelegateBase& Heap() { return *reinterpret_cast<FHeapDelegateBase*>(Storage); }
    const FHeapDelegateBase& Heap() const { return *reinterpret_cast<const FHeapDelegateBase*>(Storage); }

    int32_t& DelegateSize() { return DelegateBase::UsesHeapAllocator() ? Heap().DelegateSize : Inline().DelegateSize; }
    int32_t DelegateSize() const { return DelegateBase::UsesHeapAllocator() ? Heap().DelegateSize : Inline().DelegateSize; }

    void* GetAllocation() const
    {
        if (DelegateBase::UsesHeapAllocator())
        {
            return Heap().Data;
        }
        const FInlineDelegateBase::FDelegateAllocator& Allocator = Inline().DelegateAllocator;
        return Allocator.SecondaryData ? Allocator.SecondaryData : const_cast<FAlignedInlineDelegateType*>(Allocator.InlineData);
    }

    void ResizeAllocation(int32_t NumElements)
    {
        if (DelegateBase::UsesHeapAllocator())
        {
            // FHeapAllocator::ResizeAllocation: a Realloc to 0 frees.
            if (Heap().Data || NumElements)
            {
                Heap().Data = FMemory::Realloc(Heap().Data, NumElements * sizeof(FAlignedInlineDelegateType));
            }
        }
        else if (NumElements <= NumInlineElements)
        {
            if (Inline().DelegateAllocator.SecondaryData)
            {
                FMemory::Free(Inline().DelegateAllocator.SecondaryData);
                Inline().DelegateAllocator.SecondaryData = nullptr;
            }
        }
        else
        {
            Inline().DelegateAllocator.SecondaryData = FMemory::Realloc(Inline().DelegateAllocator.SecondaryData, NumElements * sizeof(FAlignedInlineDelegateType), alignof(FAlignedInlineDelegateType));
        }
    }

    alignas(16) uint8_t Storage[sizeof(FInlineDelegateBase)] = {};

    friend struct FDelegateBaseLayout;
};

struct FDelegateBaseLayout
{
    static_assert(sizeof(FDelegateBase) == 0x40);
    static_assert(sizeof(FDelegateBase::FInlineDelegateBase) == 0x40);
    static_assert(offsetof(FDelegateBase::FInlineDelegateBase, DelegateSize) == 0x30);
    static_assert(sizeof(FDelegateBase::FHeapDelegateBase) == 0x10);
    static_assert(offsetof(FDelegateBase::FHeapDelegateBase, DelegateSize) == 0x08);
};

inline void* operator new(size_t Size, FDelegateBase& Base)
{
    return Base.Allocate(static_cast<int32_t>(Size));
}

inline void operator delete(void*, FDelegateBase&)
{
}
