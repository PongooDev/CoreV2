#pragma once

#include "Log.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

#include <cstdint>
#include <algorithm>
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
        explicit FParams(UFunction* InFunction, bool bInitializeParms = false)
            : Function(InFunction)
            , bInitialized(InFunction && bInitializeParms)
        {
            if (Function)
            {
                // The whole frame: InitializeStruct and DestroyStruct cover a function's local variables too
                // (UStruct::GetStructureSize, 1.8.1 0x13CAB70), and ProcessEvent reads only the first ParmsSize bytes.
                Buffer.assign(std::max<size_t>(Function->ParmsSize, static_cast<size_t>(Function->GetStructureSize())), 0);
            }
            if (bInitialized)
            {
                Function->InitializeStruct(Buffer.data());
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
            if (bInitialized)
            {
                Function->DestroyStruct(Buffer.data());
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
            if (bInitialized)
            {
                *Slot = Value;
                return true;
            }
            new (Slot) T(Value);
            Destructors.push_back([Slot] { std::destroy_at(Slot); });
            return true;
        }

        template <typename T>
        T* Get(const wchar_t* Name)
        {
            return Find<T>(Name);
        }

        template <typename T>
        T Take(const wchar_t* Name)
        {
            T* Slot = Find<T>(Name);
            return Slot ? T(std::move(*Slot)) : T();
        }

        // A parameter's storage, whatever its size: for struct values Core only passes between calls (refcounted
        // handles such as FGameplayEffectSpecHandle), which CopyStruct copies through their script struct.
        void* GetAddress(const wchar_t* Name)
        {
            UProperty* Property = FindProperty(Name);
            return Property ? Buffer.data() + Property->GetOffset_ForInternal() : nullptr;
        }

        // Copies Source into a struct parameter with the struct's own copy (the buffer must be initialized).
        bool CopyStruct(const wchar_t* Name, const void* Source)
        {
            UProperty* Property = FindProperty(Name);
            if (!Property || !bInitialized || !Source || !Property->GetClass()->HasAnyCastFlag(CASTCLASS_UStructProperty))
            {
                Log::Error(L"Reflection: parameter {} of {} is not an initialized struct", Name, Function ? Function->GetName().ToStdString() : std::wstring(L"(none)"));
                return false;
            }
            UScriptStruct* Struct = static_cast<UStructProperty*>(Property)->Struct;
            Struct->CopyScriptStruct(Buffer.data() + Property->GetOffset_ForInternal(), Source);
            return true;
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
        UProperty* FindProperty(const wchar_t* Name)
        {
            UProperty* Property = Function ? Function->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
            if (!Property)
            {
                Log::Error(L"Reflection: parameter {} of {} was not found", Name, Function ? Function->GetName().ToStdString() : std::wstring(L"(none)"));
            }
            return Property;
        }

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
        bool bInitialized = false;
        std::vector<uint8_t> Buffer;
        std::vector<std::function<void()>> Destructors;
    };
}
