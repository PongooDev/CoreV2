#include "pch.h"
#include "HAL/IConsoleManager.h"
#include "Finder.h"
#include "Natives.h"

IConsoleManager& IConsoleManager::Get()
{
    if (!*Natives::IConsoleManager_Singleton)
    {
        Natives::IConsoleManager_SetupSingleton();
    }
    return **Natives::IConsoleManager_Singleton;
}

void IConsoleManager::ForEachConsoleObjectThatStartsWith(const FConsoleObjectVisitor& Visitor, const wchar_t* ThatStartsWith) const
{
    if (Natives::IConsoleManager_ForEachConsoleObjectThatStartsWithIndex < 0)
    {
        Finder::ResolveConsoleManager();
    }

    using FForEach = void (*)(const IConsoleManager* This, const FConsoleObjectVisitor& Visitor, const wchar_t* ThatStartsWith);
    const FForEach ForEach = reinterpret_cast<FForEach>((*reinterpret_cast<void* const* const*>(this))[Natives::IConsoleManager_ForEachConsoleObjectThatStartsWithIndex]);
    ForEach(this, Visitor, ThatStartsWith);
}
