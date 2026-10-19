#pragma once

#include "Delegates/IDelegateInstance.h"
#include "HAL/UnrealMemory.h"

#include <cstddef>

struct alignas(16) FAlignedInlineDelegateType
{
    uint8_t Data[16];
};

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

    bool IsBound() const
    {
        IDelegateInstance* Ptr = GetDelegateInstanceProtected();
        return Ptr && Ptr->IsSafeToExecute();
    }

    void Unbind()
    {
        if (IDelegateInstance* Ptr = GetDelegateInstanceProtected())
        {
            Ptr->~IDelegateInstance();
            ResizeAllocation(0);
            DelegateSize = 0;
        }
    }

    FDelegateHandle GetHandle() const
    {
        IDelegateInstance* Ptr = GetDelegateInstanceProtected();
        return Ptr ? Ptr->GetHandle() : FDelegateHandle();
    }

    IDelegateInstance* GetDelegateInstanceProtected() const
    {
        return DelegateSize ? static_cast<IDelegateInstance*>(GetAllocation()) : nullptr;
    }

    void* Allocate(int32_t Size)
    {
        if (IDelegateInstance* CurrentInstance = GetDelegateInstanceProtected())
        {
            CurrentInstance->~IDelegateInstance();
        }

        const int32_t NewDelegateSize = (Size + static_cast<int32_t>(sizeof(FAlignedInlineDelegateType)) - 1) / static_cast<int32_t>(sizeof(FAlignedInlineDelegateType));
        if (DelegateSize != NewDelegateSize)
        {
            ResizeAllocation(NewDelegateSize);
            DelegateSize = NewDelegateSize;
        }
        return GetAllocation();
    }

private:
    static constexpr int32_t NumInlineElements = 2;

    struct alignas(16) FDelegateAllocator
    {
        FAlignedInlineDelegateType InlineData[NumInlineElements] = {};
        void* SecondaryData = nullptr;
    };

    void* GetAllocation() const
    {
        return DelegateAllocator.SecondaryData ? DelegateAllocator.SecondaryData : const_cast<FAlignedInlineDelegateType*>(DelegateAllocator.InlineData);
    }

    void ResizeAllocation(int32_t NumElements)
    {
        if (NumElements <= NumInlineElements)
        {
            if (DelegateAllocator.SecondaryData)
            {
                FMemory::Free(DelegateAllocator.SecondaryData);
                DelegateAllocator.SecondaryData = nullptr;
            }
        }
        else
        {
            DelegateAllocator.SecondaryData = FMemory::Realloc(DelegateAllocator.SecondaryData, NumElements * sizeof(FAlignedInlineDelegateType), alignof(FAlignedInlineDelegateType));
        }
    }

    FDelegateAllocator DelegateAllocator;
    int32_t DelegateSize = 0;

    friend struct FDelegateBaseLayout;
};

struct FDelegateBaseLayout
{
    static_assert(sizeof(FDelegateBase) == 0x40);
    static_assert(offsetof(FDelegateBase, DelegateSize) == 0x30);
};

inline void* operator new(size_t Size, FDelegateBase& Base)
{
    return Base.Allocate(static_cast<int32_t>(Size));
}

inline void operator delete(void*, FDelegateBase&)
{
}
