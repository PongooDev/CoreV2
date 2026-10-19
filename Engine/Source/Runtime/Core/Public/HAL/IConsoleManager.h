#pragma once

#include "Delegates/Delegate.h"

#include <cstdint>

enum EConsoleVariableFlags
{
    ECVF_Default = 0x0,
    ECVF_Cheat = 0x1,
    ECVF_ReadOnly = 0x4,
    ECVF_Unregistered = 0x8,
};

class IConsoleVariable;

class IConsoleObject
{
public:
    virtual ~IConsoleObject() = default;
    virtual const wchar_t* GetHelp() const = 0;
    virtual void SetHelp(const wchar_t* Value) = 0;
    virtual EConsoleVariableFlags GetFlags() const = 0;
    virtual void SetFlags(const EConsoleVariableFlags Value) = 0;

    virtual IConsoleVariable* AsVariable()
    {
        return nullptr;
    }

    bool TestFlags(const EConsoleVariableFlags Value) const
    {
        return (static_cast<uint32_t>(GetFlags()) & static_cast<uint32_t>(Value)) != 0;
    }
};

using FConsoleObjectVisitor = TBaseDelegate<void, const wchar_t*, IConsoleObject*>;

class IConsoleManager
{
public:
    static IConsoleManager& Get();

    void ForEachConsoleObjectThatStartsWith(const FConsoleObjectVisitor& Visitor, const wchar_t* ThatStartsWith = L"") const;
};
