#pragma once

#include "Templates/SharedPointerInternals.h"

#include <cstdint>
#include <intrin.h>
#include <utility>

namespace SharedPointerInternals
{
    class FReferenceControllerBase
    {
    public:
        virtual void DestroyObject() = 0;
        virtual ~FReferenceControllerBase() = default;

        int32_t SharedReferenceCount = 1;
        int32_t WeakReferenceCount = 1;
    };

    template <ESPMode Mode>
    struct FReferenceControllerOps
    {
        static void AddSharedReference(FReferenceControllerBase* ReferenceController)
        {
            if constexpr (Mode == ESPMode::ThreadSafe)
            {
                _InterlockedIncrement(reinterpret_cast<volatile long*>(&ReferenceController->SharedReferenceCount));
            }
            else
            {
                ++ReferenceController->SharedReferenceCount;
            }
        }

        static void ReleaseSharedReference(FReferenceControllerBase* ReferenceController)
        {
            if (Decrement(ReferenceController->SharedReferenceCount) == 0)
            {
                ReferenceController->DestroyObject();
                ReleaseWeakReference(ReferenceController);
            }
        }

        static void ReleaseWeakReference(FReferenceControllerBase* ReferenceController)
        {
            if (Decrement(ReferenceController->WeakReferenceCount) == 0)
            {
                delete ReferenceController;
            }
        }

    private:
        static int32_t Decrement(int32_t& Count)
        {
            if constexpr (Mode == ESPMode::ThreadSafe)
            {
                return _InterlockedDecrement(reinterpret_cast<volatile long*>(&Count));
            }
            else
            {
                return --Count;
            }
        }
    };
}

template <class ObjectType, ESPMode Mode>
class TSharedPtr
{
    using FOps = SharedPointerInternals::FReferenceControllerOps<Mode>;

public:
    TSharedPtr() = default;

    TSharedPtr(const TSharedPtr& Other)
        : Object(Other.Object), ReferenceController(Other.ReferenceController)
    {
        if (ReferenceController)
        {
            FOps::AddSharedReference(ReferenceController);
        }
    }

    TSharedPtr(TSharedPtr&& Other) noexcept
        : Object(Other.Object), ReferenceController(Other.ReferenceController)
    {
        Other.Object = nullptr;
        Other.ReferenceController = nullptr;
    }

    ~TSharedPtr()
    {
        if (ReferenceController)
        {
            FOps::ReleaseSharedReference(ReferenceController);
        }
    }

    TSharedPtr& operator=(TSharedPtr Other) noexcept
    {
        std::swap(Object, Other.Object);
        std::swap(ReferenceController, Other.ReferenceController);
        return *this;
    }

    ObjectType* Get() const
    {
        return Object;
    }

    bool IsValid() const
    {
        return Object != nullptr;
    }

    ObjectType* operator->() const
    {
        return Object;
    }

    ObjectType& operator*() const
    {
        return *Object;
    }

private:
    ObjectType* Object = nullptr;
    SharedPointerInternals::FReferenceControllerBase* ReferenceController = nullptr;
};

static_assert(sizeof(TSharedPtr<int, ESPMode::Fast>) == 0x10);
