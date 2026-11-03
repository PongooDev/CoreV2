#pragma once

#include "Delegates/DelegateBase.h"

#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

template <typename FuncType>
class IBaseDelegateInstance;

template <typename RetType, typename... ParamTypes>
class IBaseDelegateInstance<RetType(ParamTypes...)> : public IDelegateInstance
{
public:
    virtual void CreateCopy(FDelegateBase& Base) = 0;
    virtual bool IsSameFunction(const IBaseDelegateInstance& InOtherDelegate) const = 0;
    virtual RetType Execute(ParamTypes... Params) const = 0;
    virtual bool ExecuteIfSafe(ParamTypes... Params) const = 0;
};

namespace DelegateInstance
{
    // IBaseDelegateInstance's virtuals at the build's slots (see EDelegateInstanceSlot). Core's delegates return void.
    template <typename RetType, typename... ParamTypes>
    void CreateCopy(IBaseDelegateInstance<RetType(ParamTypes...)>* Instance, FDelegateBase& Base)
    {
        CallVirtual<void, IBaseDelegateInstance<RetType(ParamTypes...)>, FDelegateBase&>(Instance, Slot(EDelegateInstanceSlot::CreateCopy), Base);
    }

    template <typename RetType, typename... ParamTypes>
    RetType Execute(const IBaseDelegateInstance<RetType(ParamTypes...)>* Instance, std::type_identity_t<ParamTypes>... Params)
    {
        static_assert(std::is_void_v<RetType> || std::is_scalar_v<RetType>, "a class return value is passed through a hidden pointer");
        return CallVirtual<RetType, const IBaseDelegateInstance<RetType(ParamTypes...)>, ParamTypes...>(Instance, Slot(EDelegateInstanceSlot::Execute), Params...);
    }

    template <typename RetType, typename... ParamTypes>
    bool ExecuteIfSafe(const IBaseDelegateInstance<RetType(ParamTypes...)>* Instance, std::type_identity_t<ParamTypes>... Params)
    {
        return CallVirtual<bool, const IBaseDelegateInstance<RetType(ParamTypes...)>, ParamTypes...>(Instance, Slot(EDelegateInstanceSlot::ExecuteIfSafe), Params...);
    }
}

template <typename FuncType, typename... VarTypes>
class TBaseStaticDelegateInstance;

template <typename RetType, typename... ParamTypes, typename... VarTypes>
class TBaseStaticDelegateInstance<RetType(ParamTypes...), VarTypes...> : public IBaseDelegateInstance<RetType(ParamTypes...)>
{
public:
    using FFuncPtr = RetType (*)(ParamTypes..., VarTypes...);

    TBaseStaticDelegateInstance(FFuncPtr InStaticFuncPtr, VarTypes... Vars)
        : StaticFuncPtr(InStaticFuncPtr), Payload(Vars...), Handle(FDelegateHandle::GenerateNewHandle)
    {
        UseBuildVTable();
    }

    TBaseStaticDelegateInstance(const TBaseStaticDelegateInstance& Other)
        : StaticFuncPtr(Other.StaticFuncPtr), Payload(Other.Payload), Handle(Other.Handle)
    {
        UseBuildVTable();
    }

    FName GetFunctionName() const final { return FName(); }
    UObject* GetUObject() const final { return nullptr; }
    const void* GetRawMethodPtr() const final { return reinterpret_cast<const void*>(StaticFuncPtr); }
    const void* GetRawUserObject() const final { return nullptr; }
    EDelegateInstanceType::Type GetType() const final { return EDelegateInstanceType::Raw; }
    bool HasSameObject(const void*) const final { return false; }
    // Not IDelegateInstance's !IsSafeToExecute(): that is a virtual call, and 4.20's vtable has Execute at its slot.
    bool IsCompactable() const final { return !TBaseStaticDelegateInstance::IsSafeToExecute(); }
    bool IsSafeToExecute() const final { return true; }
    FDelegateHandle GetHandle() const final { return Handle; }

    void CreateCopy(FDelegateBase& Base) final
    {
        new (Base) TBaseStaticDelegateInstance(*this);
    }

    bool IsSameFunction(const IBaseDelegateInstance<RetType(ParamTypes...)>& InOtherDelegate) const final
    {
        return InOtherDelegate.GetType() == EDelegateInstanceType::Raw && InOtherDelegate.GetRawMethodPtr() == TBaseStaticDelegateInstance::GetRawMethodPtr();
    }

    RetType Execute(ParamTypes... Params) const final
    {
        return std::apply([&](auto&... Vars) { return StaticFuncPtr(Params..., Vars...); }, Payload);
    }

    bool ExecuteIfSafe(ParamTypes... Params) const final
    {
        TBaseStaticDelegateInstance::Execute(Params...);
        return true;
    }

private:
    // On 4.20 the engine calls this instance by 4.20's slots: point it at its own functions in that order.
    void UseBuildVTable()
    {
        if (DelegateInstance::UsesLayout420())
        {
            void* const** const VTablePointer = reinterpret_cast<void* const**>(this);
            *VTablePointer = DelegateInstance::BuildVTable<TBaseStaticDelegateInstance>(*VTablePointer);
        }
    }

    FFuncPtr StaticFuncPtr;
    std::tuple<VarTypes...> Payload;
    FDelegateHandle Handle;
};

template <typename RetType, typename... ParamTypes>
class TBaseDelegate : public FDelegateBase
{
public:
    using FDelegateInstance = IBaseDelegateInstance<RetType(ParamTypes...)>;

    template <typename... VarTypes>
    static TBaseDelegate CreateStatic(typename TBaseStaticDelegateInstance<RetType(ParamTypes...), VarTypes...>::FFuncPtr InFunc, VarTypes... Vars)
    {
        TBaseDelegate Result;
        new (Result) TBaseStaticDelegateInstance<RetType(ParamTypes...), VarTypes...>(InFunc, Vars...);
        return Result;
    }

    TBaseDelegate() = default;

    TBaseDelegate(TBaseDelegate&& Other) noexcept
    {
        if (FDelegateInstance* OtherInstance = Other.GetDelegateInstance())
        {
            DelegateInstance::CreateCopy(OtherInstance, *this);
            Other.Unbind();
        }
    }

    RetType Execute(ParamTypes... Params) const
    {
        return DelegateInstance::Execute(GetDelegateInstance(), Params...);
    }

    bool ExecuteIfBound(ParamTypes... Params) const
    {
        if (IsBound())
        {
            Execute(Params...);
            return true;
        }
        return false;
    }

private:
    FDelegateInstance* GetDelegateInstance() const
    {
        return static_cast<FDelegateInstance*>(GetDelegateInstanceProtected());
    }
};

template <typename... ParamTypes>
class TMulticastDelegate
{
public:
    using FDelegateInstance = IBaseDelegateInstance<void(ParamTypes...)>;

    TMulticastDelegate() = delete;

    void Broadcast(ParamTypes... Params) const
    {
        bool NeedsCompaction = false;

        ++InvocationListLockCount;
        for (int32_t InvocationListIndex = InvocationList.ArrayNum - 1; InvocationListIndex >= 0; --InvocationListIndex)
        {
            FDelegateInstance* DelegateInstance = static_cast<FDelegateInstance*>(InvocationList.GetData()[InvocationListIndex].GetDelegateInstanceProtected());
            if (!DelegateInstance || !::DelegateInstance::ExecuteIfSafe(DelegateInstance, Params...))
            {
                NeedsCompaction = true;
            }
        }
        --InvocationListLockCount;

        if (NeedsCompaction)
        {
            const_cast<TMulticastDelegate*>(this)->CompactInvocationList();
        }
    }

private:
    struct alignas(16) FInvocationList
    {
        struct alignas(16) FAllocator
        {
            alignas(16) uint8_t InlineData[sizeof(FDelegateBase)];
            FDelegateBase* SecondaryData;
        };

        FDelegateBase* GetData() const
        {
            return Allocator.SecondaryData ? Allocator.SecondaryData : reinterpret_cast<FDelegateBase*>(const_cast<uint8_t*>(Allocator.InlineData));
        }

        FAllocator Allocator;
        int32_t ArrayNum;
        int32_t ArrayMax;
    };

    void CompactInvocationList()
    {
        if (InvocationListLockCount > 0)
        {
            return;
        }

        const int32_t OldNumItems = InvocationList.ArrayNum;
        for (int32_t Index = 0; Index < InvocationList.ArrayNum;)
        {
            FDelegateBase& DelegateBase = InvocationList.GetData()[Index];
            IDelegateInstance* DelegateInstance = DelegateBase.GetDelegateInstanceProtected();
            if (!DelegateInstance || ::DelegateInstance::IsCompactable(DelegateInstance))
            {
                DelegateBase.~FDelegateBase();
                if (Index != InvocationList.ArrayNum - 1)
                {
                    memcpy(static_cast<void*>(&DelegateBase), &InvocationList.GetData()[InvocationList.ArrayNum - 1], sizeof(FDelegateBase));
                }
                --InvocationList.ArrayNum;
            }
            else
            {
                ++Index;
            }
        }

        CompactionThreshold = InvocationList.ArrayNum * 2 > 2 ? InvocationList.ArrayNum * 2 : 2;
        if (OldNumItems > CompactionThreshold && InvocationList.Allocator.SecondaryData && InvocationList.ArrayNum <= 1)
        {
            if (InvocationList.ArrayNum == 1)
            {
                memcpy(InvocationList.Allocator.InlineData, InvocationList.Allocator.SecondaryData, sizeof(FDelegateBase));
            }
            FMemory::Free(InvocationList.Allocator.SecondaryData);
            InvocationList.Allocator.SecondaryData = nullptr;
            InvocationList.ArrayMax = 1;
        }
    }

    FInvocationList InvocationList;
    int32_t CompactionThreshold;
    mutable int32_t InvocationListLockCount;

    friend struct FMulticastDelegateLayout;
};

struct FMulticastDelegateLayout
{
    static_assert(offsetof(TMulticastDelegate<int32_t>, InvocationList.ArrayNum) == 0x50);
    static_assert(offsetof(TMulticastDelegate<int32_t>, CompactionThreshold) == 0x60);
    static_assert(offsetof(TMulticastDelegate<int32_t>, InvocationListLockCount) == 0x64);
    static_assert(sizeof(TMulticastDelegate<int32_t>) == 0x70);
};
