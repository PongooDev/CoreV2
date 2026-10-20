#pragma once

#include "Log.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <vector>

namespace Reflection
{
    class FParams
    {
    public:
        explicit FParams(UFunction* InFunction)
            : Function(InFunction)
        {
            if (Function)
            {
                Buffer.assign(Function->ParmsSize, 0);
            }
        }

        FParams(const FParams&) = delete;
        FParams& operator=(const FParams&) = delete;

        ~FParams()
        {
            for (const std::function<void()>& Destroy : Destructors)
            {
                Destroy();
            }
        }

        template <typename T>
        bool Set(const wchar_t* Name, const T& Value)
        {
            T* Slot = Find<T>(Name);
            if (!Slot)
            {
                return false;
            }
            new (Slot) T(Value);
            Destructors.push_back([Slot] { std::destroy_at(Slot); });
            return true;
        }

        template <typename T>
        T Take(const wchar_t* Name)
        {
            T* Slot = Find<T>(Name);
            return Slot ? T(std::move(*Slot)) : T();
        }

        bool Relocate(const wchar_t* Name, void* Destination, size_t Size)
        {
            UProperty* Property = Function ? Function->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
            if (!Property || static_cast<size_t>(Property->ElementSize) != Size)
            {
                Log::Error(L"Reflection: parameter {} of {} was not found or has a different size", Name, Function ? Function->GetName().ToStdString() : std::wstring(L"(none)"));
                return false;
            }
            std::memcpy(Destination, Buffer.data() + Property->GetOffset_ForInternal(), Size);
            return true;
        }

        bool Call(UObject* Object)
        {
            if (!Function || !Object)
            {
                return false;
            }
            Object->ProcessEvent(Function, Buffer.data());
            return true;
        }

    private:
        template <typename T>
        T* Find(const wchar_t* Name)
        {
            UProperty* Property = Function ? Function->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
            if (!Property || static_cast<size_t>(Property->ElementSize) != sizeof(T))
            {
                Log::Error(L"Reflection: parameter {} of {} was not found or has a different size", Name, Function ? Function->GetName().ToStdString() : std::wstring(L"(none)"));
                return nullptr;
            }
            return reinterpret_cast<T*>(Buffer.data() + Property->GetOffset_ForInternal());
        }

        UFunction* Function;
        std::vector<uint8_t> Buffer;
        std::vector<std::function<void()>> Destructors;
    };
}
