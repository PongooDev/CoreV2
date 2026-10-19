#include "pch.h"
#include "Misc/App.h"
#include "Hooks.h"
#include "Natives.h"

void FApp::Hook()
{
    if (uint8_t* OneThread = Sites::ShouldUseThreadingForPerformanceDedicatedCheck)
    {
        Hooks::SkipBranch(L"FApp::ShouldUseThreadingForPerformance IsRunningDedicatedServer", OneThread);
    }
}
