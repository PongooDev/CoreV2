#pragma once

#include "Containers/UnrealString.h"
#include "CoreGlobals.h"
#include "Delegates/Delegate.h"

#include <cstdint>
#include <type_traits>

enum EConsoleVariableFlags
{
    ECVF_Default = 0x0,
    ECVF_Cheat = 0x1,
    ECVF_ReadOnly = 0x4,
    ECVF_Unregistered = 0x8,
    ECVF_CreatedFromIni = 0x10,
    ECVF_RenderThreadSafe = 0x20,
    ECVF_Scalability = 0x40,
    ECVF_ScalabilityGroup = 0x80,
    ECVF_SetByMask = 0xff000000,
    ECVF_SetByConstructor = 0x00000000,
    ECVF_SetByScalability = 0x01000000,
    ECVF_SetByGameSetting = 0x02000000,
    ECVF_SetByProjectSetting = 0x03000000,
    ECVF_SetByDeviceProfile = 0x04000000,
    ECVF_SetBySystemSettingsIni = 0x05000000,
    ECVF_SetByConsoleVariablesIni = 0x06000000,
    ECVF_SetByCommandline = 0x07000000,
    ECVF_SetByCode = 0x08000000,
    ECVF_SetByConsole = 0x09000000,
};

class IConsoleVariable;
struct IConsoleCommand;

template <class T>
class TConsoleVariableData;

using FConsoleVariableDelegate = TBaseDelegate<void, IConsoleVariable*>;

class IConsoleObject
{
public:
    virtual ~IConsoleObject() = default;
    virtual const wchar_t* GetHelp() const = 0;
    virtual void SetHelp(const wchar_t* Value) = 0;
    virtual EConsoleVariableFlags GetFlags() const = 0;
    virtual void SetFlags(const EConsoleVariableFlags Value) = 0;

    void ClearFlags(const EConsoleVariableFlags Value)
    {
        SetFlags(static_cast<EConsoleVariableFlags>(static_cast<uint32_t>(GetFlags()) & ~static_cast<uint32_t>(Value)));
    }

    bool TestFlags(const EConsoleVariableFlags Value) const
    {
        return (static_cast<uint32_t>(GetFlags()) & static_cast<uint32_t>(Value)) != 0;
    }

    virtual IConsoleVariable* AsVariable()
    {
        return nullptr;
    }

    virtual TConsoleVariableData<int32_t>* AsVariableInt()
    {
        return nullptr;
    }

    virtual TConsoleVariableData<float>* AsVariableFloat()
    {
        return nullptr;
    }

    virtual TConsoleVariableData<FString>* AsVariableString()
    {
        return nullptr;
    }

    virtual IConsoleCommand* AsCommand()
    {
        return nullptr;
    }

private:
    virtual void Release() = 0;
};

class IConsoleVariable : public IConsoleObject
{
public:
    virtual void Set(const wchar_t* InValue, EConsoleVariableFlags SetBy = ECVF_SetByCode) = 0;
    virtual int32_t GetInt() const = 0;
    virtual float GetFloat() const = 0;
    virtual FString GetString() const = 0;
    virtual void SetOnChangedCallback(const FConsoleVariableDelegate& Callback) = 0;

    void Set(int32_t InValue, EConsoleVariableFlags SetBy = ECVF_SetByCode)
    {
        Set(*FString::Printf(L"%d", InValue), SetBy);
    }

    void Set(float InValue, EConsoleVariableFlags SetBy = ECVF_SetByCode)
    {
        Set(*FString::Printf(L"%g", InValue), SetBy);
    }

    void SetWithCurrentPriority(int32_t InValue)
    {
        Set(InValue, static_cast<EConsoleVariableFlags>(GetFlags() & ECVF_SetByMask));
    }

    void SetWithCurrentPriority(float InValue)
    {
        Set(InValue, static_cast<EConsoleVariableFlags>(GetFlags() & ECVF_SetByMask));
    }
};

using FConsoleObjectVisitor = TBaseDelegate<void, const wchar_t*, IConsoleObject*>;

class IConsoleManager
{
public:
    static IConsoleManager& Get();

    IConsoleVariable* FindConsoleVariable(const wchar_t* Name) const;
    void ForEachConsoleObjectThatStartsWith(const FConsoleObjectVisitor& Visitor, const wchar_t* ThatStartsWith = L"") const;
};

template <class T>
class TConsoleVariableData
{
public:
    TConsoleVariableData(const T DefaultValue)
    {
        for (T& Value : ShadowedValue)
        {
            Value = DefaultValue;
        }
    }

    T GetValueOnGameThread() const
    {
        return ShadowedValue[0];
    }

    T GetValueOnRenderThread() const
    {
        return ShadowedValue[1];
    }

    T GetValueOnAnyThread(bool bForceGameThread = false) const
    {
        return ShadowedValue[GetShadowIndex(bForceGameThread)];
    }

private:
    static uint32_t GetShadowIndex(bool bForceGameThread = false)
    {
        if (bForceGameThread)
        {
            return 0;
        }

        return IsInGameThread() ? 0 : 1;
    }

    T ShadowedValue[2];
};

class FAutoConsoleObject
{
protected:
    FAutoConsoleObject(const wchar_t* InName, const wchar_t* InHelp, uint32_t InFlags)
        : Name(InName), Help(InHelp), Flags(InFlags)
    {
    }

    FAutoConsoleObject(const FAutoConsoleObject&) = delete;
    FAutoConsoleObject& operator=(const FAutoConsoleObject&) = delete;

    IConsoleVariable* AsVariable() const;

private:
    const wchar_t* Name;
    const wchar_t* Help;
    uint32_t Flags;
    mutable IConsoleVariable* Target = nullptr;
    mutable bool bSearched = false;
};

template <class T>
class TAutoConsoleVariable : public FAutoConsoleObject
{
public:
    TAutoConsoleVariable(const wchar_t* Name, const T& DefaultValue, const wchar_t* Help, uint32_t Flags = ECVF_Default)
        : FAutoConsoleObject(Name, Help, Flags), Unregistered(DefaultValue)
    {
    }

    T GetValueOnGameThread() const
    {
        return GetRef()->GetValueOnGameThread();
    }

    T GetValueOnRenderThread() const
    {
        return GetRef()->GetValueOnRenderThread();
    }

    T GetValueOnAnyThread(bool bForceGameThread = false) const
    {
        return GetRef()->GetValueOnAnyThread(bForceGameThread);
    }

    IConsoleVariable& operator*() const
    {
        return *AsVariable();
    }

    IConsoleVariable* operator->() const
    {
        return AsVariable();
    }

private:
    const TConsoleVariableData<T>* GetRef() const
    {
        if (!Ref)
        {
            IConsoleVariable* Variable = AsVariable();
            if (!Variable)
            {
                Ref = &Unregistered;
            }
            else if constexpr (std::is_same_v<T, int32_t>)
            {
                Ref = Variable->AsVariableInt();
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                Ref = Variable->AsVariableFloat();
            }
            else
            {
                Ref = Variable->AsVariableString();
            }
        }
        return Ref;
    }

    mutable const TConsoleVariableData<T>* Ref = nullptr;
    TConsoleVariableData<T> Unregistered;
};
