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
