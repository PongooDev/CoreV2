#include "pch.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Finder.h"
#include "Natives.h"
#include "Templates/SharedPointer.h"

FTextLocalizationManager& FTextLocalizationManager::Get()
{
    if (!Natives::FTextLocalizationManager_FindDisplayString)
    {
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
