#include "pch.h"
#include "HAL/ThreadHeartBeat.h"
#include "Natives.h"

FThreadHeartBeat* FThreadHeartBeat::GetNoInit()
{
    return IsAvailable() ? Natives::FThreadHeartBeat_GetNoInit() : nullptr;
}

bool FThreadHeartBeat::IsAvailable()
{
    return Natives::FThreadHeartBeat_GetNoInit && Natives::FThreadHeartBeat_SuspendHeartBeat && Natives::FThreadHeartBeat_ResumeHeartBeat;
}

void FThreadHeartBeat::SuspendHeartBeat(bool bAllThreads)
{
    Natives::FThreadHeartBeat_SuspendHeartBeat(this, bAllThreads);
}

void FThreadHeartBeat::ResumeHeartBeat(bool bAllThreads)
{
    Natives::FThreadHeartBeat_ResumeHeartBeat(this, bAllThreads);
}
