#pragma once

#include "Delegates/IDelegateInstance.h"

#include <type_traits>

class FDelegateBase;

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
