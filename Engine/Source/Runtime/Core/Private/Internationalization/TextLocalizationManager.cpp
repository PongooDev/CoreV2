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
