#include "pch.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Finder.h"
#include "Natives.h"
#include "Templates/SharedPointer.h"

FTextLocalizationManager& FTextLocalizationManager::Get()
{
    // Resolved once: on 5.0 FindDisplayString stays null (it's inlined), and the console asks for every show flag (#343).
    static bool bResolved = false;
    if (!bResolved && !Natives::FTextLocalizationManager_FindDisplayString)
    {
        bResolved = true;
        Finder::ResolveTextLocalizationManager();
    }
    // 7.20 keeps the manager in its own Get's function-local static (TLazySingleton), so the game's Get returns it (#496).
    if (Natives::FTextLocalizationManager_Get)
    {
        return Natives::FTextLocalizationManager_Get();
    }
    if (!Natives::GTextLocalizationManager)
    {
        // Neither was found (the finder logs it). FindDisplayString is only resolved together with a manager, so this
        // empty stand-in finds no display string: the console lists the show flags without names instead of crashing.
        static FTextLocalizationManager Unresolved;
        return Unresolved;
    }
    return **Natives::GTextLocalizationManager;
}

bool FTextLocalizationManager::FindDisplayString(const FString& Namespace, const FString& Key, FString& OutDisplayString, const FString* SourceString) const
{
    if (!Natives::FTextLocalizationManager_FindDisplayString)
    {
        return false;
    }

    TSharedPtr<FString, ESPMode::ThreadSafe> DisplayString;
    Natives::FTextLocalizationManager_FindDisplayString(this, &DisplayString, &Namespace, &Key, SourceString);
    if (!DisplayString.IsValid())
    {
        return false;
    }

    OutDisplayString = *DisplayString;
    return true;
}
