#pragma once

#include "Delegates/Delegate.h"
#include "Finder.h"
#include "Natives.h"

// Epic's FCoreDelegates (CoreDelegates.h): the engine's global delegates, reached through the build's statics.
class FCoreDelegates
{
public:
    // Epic's OnFEngineLoopInitComplete: broadcast once, as the last step of FEngineLoop::Init (after GEngine->Start has
    // loaded the default map). Null where the build's isn't found (Finder::ResolveEngineLoopInitComplete, logged once).
    static FSimpleMulticastDelegate* GetOnFEngineLoopInitComplete()
    {
        return Finder::ResolveEngineLoopInitComplete() ? static_cast<FSimpleMulticastDelegate*>(Natives::FCoreDelegates_OnFEngineLoopInitComplete) : nullptr;
    }
};
