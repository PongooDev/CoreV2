#pragma once

#include "Containers/UnrealString.h"
#include "Natives.h"

struct FEngineShowFlags
{
    template <class T>
    static void IterateAllFlags(T& Sink)
    {
        for (const Sites::FShowFlag& ShowFlag : Sites::ShowFlags)
        {
            if (!Sink.OnEngineShowFlag(ShowFlag.Index, FString(ShowFlag.Name)))
            {
                return;
            }
        }
    }

    static bool FindShowFlagDisplayName(const FString& InName, FString& OutText);
};
