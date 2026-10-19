#include "pch.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Finder.h"
#include "Natives.h"

namespace
{
    struct FSharedDisplayString
    {
        FString* Object = nullptr;
        void** ReferenceController = nullptr;

        ~FSharedDisplayString()
        {
            if (!ReferenceController)
            {
                return;
            }

            void** VTable = *reinterpret_cast<void***>(ReferenceController);
            auto* Counts = reinterpret_cast<volatile long*>(reinterpret_cast<uintptr_t>(ReferenceController) + 8);
            if (_InterlockedDecrement(&Counts[0]) == 0)
            {
                reinterpret_cast<void (*)(void**)>(VTable[0])(ReferenceController);
                if (_InterlockedDecrement(&Counts[1]) == 0)
                {
                    reinterpret_cast<void (*)(void**, uint32_t)>(VTable[1])(ReferenceController, 1);
                }
            }
        }
    };
}

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

    FSharedDisplayString DisplayString;
    Natives::FTextLocalizationManager_FindDisplayString(this, &DisplayString, &Namespace, &Key, SourceString);
    if (!DisplayString.Object)
    {
        return false;
    }

    OutDisplayString = *DisplayString.Object;
    return true;
}
