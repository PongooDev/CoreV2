#include "pch.h"
#include "HAL/IConsoleManager.h"
#include "Finder.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ObjectLayout.h"

DEFINE_LOG_CATEGORY_STATIC(LogConsoleManager, Log, All);

IConsoleManager& IConsoleManager::Get()
{
    if (!*Natives::IConsoleManager_Singleton)
    {
        Natives::IConsoleManager_SetupSingleton();
    }
    return **Natives::IConsoleManager_Singleton;
}

IConsoleVariable* IConsoleManager::FindConsoleVariable(const wchar_t* Name) const
{
    if (Natives::IConsoleManager_FindConsoleVariableIndex < 0)
    {
        Finder::ResolveConsoleManager();
    }

    return CallVirtual<IConsoleVariable*>(this, Natives::IConsoleManager_FindConsoleVariableIndex, Name);
}

void IConsoleManager::ForEachConsoleObjectThatStartsWith(const FConsoleObjectVisitor& Visitor, const wchar_t* ThatStartsWith) const
{
    if (Natives::IConsoleManager_ForEachConsoleObjectThatStartsWithIndex < 0)
    {
        Finder::ResolveConsoleManager();
    }

    CallVirtual<void>(this, Natives::IConsoleManager_ForEachConsoleObjectThatStartsWithIndex, &Visitor, ThatStartsWith);
}

int32_t IConsoleObject::Slot(int32_t Index)
{
    // An int variable's AsVariableInt returns the address of its value (lea rax, [rcx+disp]; ret). 1.1-1.9.1 have it
    // right after AsVariable at slot 6 (1.9.1's net.UseAdaptiveNetUpdateFrequency initializer, 0x1A5900, calls [rdx+30h]);
    // UE 4.19's IsVariableInt takes slot 6 on 1.10 and AsVariableInt is slot 7 (call [rdx+38h] at 0x1A46D9).
    static int32_t Shift = -1;
    if (Shift < 0)
    {
        IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(L"net.UseAdaptiveNetUpdateFrequency");
        void* const* VTable = Variable ? *reinterpret_cast<void* const* const*>(Variable) : nullptr;
        const auto IsMemberAddress = [](const void* Function)
        {
            const uint8_t* Code = static_cast<const uint8_t*>(Function);
            return Code && Code[0] == 0x48 && Code[1] == 0x8D && ((Code[2] == 0x41 && Code[4] == 0xC3) || (Code[2] == 0x81 && Code[7] == 0xC3));
        };
        if (VTable && IsMemberAddress(VTable[6]))
        {
            Shift = 0;
        }
        else if (VTable && IsMemberAddress(VTable[7]))
        {
            Shift = 1;
        }
        else
        {
            UE_LOG(LogConsoleManager, Error, TEXT("IConsoleObject::AsVariableInt not found, using slot 6"));
            return Index;
        }
    }
    return Index < 6 ? Index : Index + Shift;
}

IConsoleVariable* FAutoConsoleObject::AsVariable() const
{
    if (!bSearched)
    {
        bSearched = true;
        Target = IConsoleManager::Get().FindConsoleVariable(Name);
        UE_CLOG(!Target, LogConsoleManager, Warning, TEXT("Console variable '%s' is not registered, using its default"), Name);
    }
    return Target;
}
